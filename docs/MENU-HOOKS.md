# Game menu hooks (OPTIONS -> recomp settings overlay)

Written 2026-11-20 from static analysis of `orig/R2VE01/sys/main.dol` (nothing was run). Everything marked
**VERIFIED** was read directly from DOL bytes/disassembly or the memory snapshot; everything marked
**ASSUMED** is inferred and must be confirmed by the first test run (steps at the end).

Implementation: `runtime/src/hle/game_menu_hook.cpp` (new file, picked up by the `GLOB_RECURSE src/*.cpp`).
Helper scripts written for this analysis: `tools/menu_str.py` (DOL offset/addr, `word()`), `tools/xref_data.py`
(lis/addi + data-pointer xrefs), `tools/msg_xref.py` (MsgString::get callers), `tools/callers.py` (bl callers),
`tools/menu_objs.py` (find live menu objects in `mem1.bin`).

## 1. How the game's menus work (VERIFIED unless noted)

* There is **no .bmg / message .arc** for menu text. The English UI text is a pointer table inside the DOL:
  `MsgString::get(u16 id)` = `0x80140FD4` (variant `0x80141078`), 386 ids, language from `SCGetLanguage`
  (0 = JP table `0x80303860`, 3 = FR `0x80304470`, 4 = ES `0x80304A78`, otherwise EN `0x80303E68`).
  Strings are `"Label\nDescription"`. Examples (EN): 161 `\nPause Menu`, 162 `\nResume your game.`,
  163 and 169 `\nChange game options.`, 164 `Save & Quit...`, 127 `\nSelect an option.`, 353 `CONT1P\nAdjust controller settings for player 1.`
  Icons/labels such as RESUME/OPTIONS/EXIT are also baked into the sprite sheets `comdat2_en.arc` / `titdat2_en.arc`
  (`menu_02en.tga`, `m_icon0Nen.tga`), so a new label would need text, not a sprite (see fallback B).
* Menus are data-driven (`ctl::IconMenu`, `ctl::MenuItem_*` RTTI strings at DOL file offset 0x2D4E88).
  A page = descriptor in guest `.data` (writable in guest memory):

  ```
  { flags, id|-1, msgId|textPtr, 0 }        header   (flags & 0xF.. selects layout)
  { flags, 1,     handlerFn,     0 }        record 1 (handler = per-page logic)
  { type, 0, subDescriptor|msgId, id }      item records, 16 bytes each (type 2 = submenu, 9 = "back")
  { 0xFFFF, ... }                           terminator
  ```
  Item ctor `fn_800D893C` switches on `desc[0] & 0xF` via jump table `0x802FE228` (type 2 -> `0x800D8A6C`).
* Menu object = 696 bytes (`0x2B8`), primary vtable `0x802FE440` (secondary `+0x28`, `+0x30`), item nodes have a vtable
  at `+0x3C` (submenu items: `0x802FE704`). Fields (from `fn_800DF8D8`, `fn_800D97DC`, `fn_800E83B4`):

  | offset | meaning |
  |---|---|
  | `+0x84` | flags. bit31 (`0x80000000`) set by the constructor (`0x800D9B84`, `0x800DFB38`) = "first call". bit30/29 used on close. |
  | `+0xAE` | item count (s16) |
  | `+0xB0` | selected index (s16); `+0xB2` previous selection |
  | `+0xF8` | head of the item node list (node `+0x10` = next, `+0x3C` = vtable, `+0x0C` = item id) |
  | `+0x204` | page handler fn (from descriptor record 1, `desc+0x18`) |
  | `+0x21C` | parent / owner pointer |
* `fn_800E83B4` (menu update, menu vtable slot at `+0x54`) runs input/navigation every frame and ends with
  `lwz r12,516(r24); mr r3,r24; mtctr r12; bctr` at `0x800E93E4`, i.e. **the page handler is tail-called once per
  frame with r3 = the menu object**, for as long as the page exists. `fn_800D97DC` (submenu item vtable `+0x10`, `0x800D9864..0x800DA014`)
  builds the child menu (`li r3,696; bl 0x8002C7BC`, vtable `0x802FE440`, sets bit31 at `0x800D9B84`) and calls the same handler
  immediately (`0x800DA014`: `lwz r12,516(r28) ... bctr`).

## 2. The pages we care about (VERIFIED)

| page | descriptor | header msg | handler (record 1) | role |
|---|---|---|---|---|
| **Pause menu root** | `0x802D8C58` | 161 "Pause Menu" | `0x800DE040` | items: OPTIONS (sub `0x802DA1E8`), SAVE&QUIT (`0x802D8BA8`, handler `0x800DDD2C`), QUIT (`0x802D8BF8`, `0x800DDC2C`), RESUME (type 9, msg 162) |
| **Pause -> OPTIONS** | `0x802DA1E8` | 163 | **`0x8010C1E4`** | item ids `0x01000D08`; children below |
| Title menu root | `0x802DA278` | 127 "Select an option" | `0x8010C6C8` | 1P Start (msg 130), 2P (132), **OPTIONS (sub `0x802DA158`)**, Rankings (143), back |
| **Title -> OPTIONS** | `0x802DA158` | 169 | **`0x8010BC48`** | same five children as the pause options page |
| children of both OPTIONS pages | `0x802D85B8` (CONT1P, msg 353, handler `0x800D726C`), `0x802D8658` (CONT2P, 354, `0x800DAFEC`), `0x802D89E8` (Display, msg 112, `0x800DBDAC`), `0x802D87F8` (Sound, 170, `0x800DB2F0`), `0x802D8B48` (Save settings, 136, `0x800DCAF8`), then "back" (msg 85) | | | The "Wii Remote (and Nunchuk) (1/2)" pages are below CONT1P/CONT2P. |

Live memory confirms the descriptors are unmodified at runtime (`mem1.bin` of `base_1795123066_pid39904`:
`0x802DA1E8 = 00040103 01000D08 000000A3 00000000`, `0x802DA158 = ... 000000A9`, `0x802D8C58 = 000D0103 FFFFFFFF 000000A1`).

Pause state (not needed by the hook, noted for fallback): `pauseGameNWEF(l)` `0x8008E94C` pauses Ng layers
4/12/14/19/32(/29) and `0x8008E9D4` resumes them (both called from the PAUSEWIN task: `setTcPauseWindow` `0x801217C0`,
object vtable `0x803006C8`, name string "PAUSEWIN"). `disPause`/`enaPause` (`0x800F75D4/F0`) toggle bit 2 of
`GM->+0x60->+0x44->+0x2F8` ("pause not allowed"), `GM = *(0x804B19A0)`. The world is already frozen while the pause menu
(and therefore its OPTIONS page) is open, so the overlay needs no extra world-pause; for the title there is no world.

## 3. Chosen hook (option (a): re-purpose the existing OPTIONS entry)

Wrap the two OPTIONS-page handlers (and observe the title root). When the handler is entered for a **new page
(different menu pointer, or > 250 ms since the previous call)** the player has just selected OPTIONS, so call
`settings_overlay::OpenFromGame()`, then run the original handler unchanged (so game state stays consistent).
The overlay already blocks game input while open (`PADBlockInput` + `InputBindings::SetInputBlocked`, `settings_overlay.cpp:2017`),
so the stock page cannot be operated underneath. When the overlay closes, the player sees the stock page and presses B
(known cosmetic limitation, see fallback A).

| guest fn | wrapper | notes |
|---|---|---|
| `0x8010BC48` | `GameMenu_TitleOptionsPage_8010BC48` | title OPTIONS page |
| `0x8010C1E4` | `GameMenu_PauseOptionsPage_8010C1E4` | pause OPTIONS page |
| `0x8010C6C8` | `GameMenu_TitleRoot_8010C6C8` | observation + `SIN_OPEN_OPTIONS=1` debug open after 30 frames of the title menu |

Registration is `REGISTER_NATIVE_WRAPPER_AS(0x........, fn, "name")`, **not** `PPC_NATIVE_OVERRIDE`: the translator drops the
base translation of any address named in a `PPC_NATIVE_OVERRIDE*` macro (`excludesBaseTranslation:true` in
`runtime/native-index.json`), so such a wrapper could not call the original. A `REGISTER_NATIVE_*` registration keeps the
base function (`excludesBaseTranslation:false`, precedent: `NHTTPStartup_Reentrant_HLE_801d8d30`) and the wrapper calls it with
`MKW_GUEST_NEXT(8010BC48)(ctx)` (`runtime/include/guest_function_imports.h`).
No direct `bl` callers exist for the three addresses (`python tools/callers.py 0x8010BC48 0x8010C1E4 0x8010C6C8` prints nothing); they are only reached
through the menu object's function pointer (`bctr` -> registry).

Env: `SIN_MENU_HOOK=0` disables, `SIN_MENU_HOOK_LOG=1` logs page entries, `SIN_OPEN_OPTIONS=1` opens the overlay once when the title menu is up.
Needs from the UI agent: `settings_overlay::OpenFromGame()`, `bool IsOpenFromGame()` (declared in `settings_overlay.h`; syntax-checked with a shim declaring them).

## 4. Risks / ASSUMED

1. "Handler first call after a gap = page opened". Handlers only run while their page exists (tail call from the update of that menu object), and
   the constructor calls it once. If instead the game builds the options page object earlier (prefetch) the overlay would open too early.
   Evidence against: the only constructor path found (`fn_800D97DC`) requires `r5 != 0` (`0x800D981C`) to allocate.
2. If another page type shares handler `0x8010BC48`/`0x8010C1E4` (searched: only descriptors `0x802DA158` / `0x802DA1E8` reference them, plus extab data at `0x8001D7AC`).
3. The handler of a still-open page keeps getting called while the overlay is open (we deliberately do not skip it). If an overlay opened from the
   title is closed and the user stays on the stock page, nothing re-triggers (no gap).
4. Re-entering OPTIONS within 250 ms of leaving it with the *same heap address* would not retrigger (practically impossible).
5. Base-address registry lookup (`MKW_GUEST_NEXT`) requires the base translation to exist: yes, the three functions are in `generated` (reachable from the menu code, translator strict mode, 0 unsupported).
6. A change in registered natives requires the **full rebuild chain** (`emit-native-index`, `translate-recursive`, `generate-data-init`, `emit-build-shards`):
   `ValidateGeneratedIndirectDispatchLocked` fails with "Stale generated indirect dispatch winner" if the generated indirect-dispatch table still lists these addresses as translated winners.
7. Stage modules (stg0N.rso) are not involved: menu code is entirely in main.dol (`0x800D0000-0x80110000`).

## 5. Fallbacks (not implemented; in order)

* **A. Bounce straight back / skip the stock page.** Wrapper on `fn_800D97DC` (`0x800D97DC`, item vtable `+0x10`; r3 = node, r4 = parent menu, r5 = direction/open flag;
  descriptor = `Memory::Read32(r3+8)`): when descriptor is `0x802DA158` or `0x802DA1E8` and `r5 != 0`, call `OpenFromGame()` and return `r3 = 0` (the "cannot open" return at `0x800DA434`)
  without calling the original. **ASSUMED**: `r5` and the 0 return are not known to be the confirm path; `0x800EAC80` calls it with `r5 = +1/-1` (left/right), so test that A on OPTIONS arrives with `r5 != 0` first.
  Alternative: after the overlay closes set menu `+0x84` bit30 (`0x40000000`, the value `0x800DA3E4` sets before invoking the handler on close) – unverified.
* **B. Add a new menu entry.** Descriptors are plain guest data. The OPTIONS page (`0x802DA158/0x802DA1E8`) has 5 item records + back; repurpose one record (e.g. Save settings `{2,0,0x802D8B48,0x01001100}`)
  or point an item at a new descriptor built in guest memory (`MEM1` heap/`0x817...`), and overwrite an unused EN message pointer (`0x80303E68 + 4*id`, e.g. id 131 duplicates 132) with a pointer to
  our own `"Recomp Settings\nOpen the PC port settings."`; item `+8` msg id selects the text. Then use the sub-descriptor's handler (record 1) as our hook.
* **C. Per-page deep link.** The five children have their own handlers: `0x800D726C` CONT1P, `0x800DAFEC` CONT2P, `0x800DBDAC` Display, `0x800DB2F0` Sound, `0x800DCAF8` Save: the same wrapper pattern can open the overlay on the matching tab (Controls / Graphics / Audio) if `OpenFromGame(tab)` is added.
* **D. Pause-menu hint instead.** Wrap root handler `0x800DE040` and open the overlay on a free button (e.g. R3) with a log/overlay hint; text via `MsgString` pointer overwrite (see B).

## 6. Test plan for the lead

1. Full rebuild chain (STATUS.md "Build"); confirm `runtime/native-index.json` now lists `0x8010BC48`, `0x8010C1E4`, `0x8010C6C8` with `"modOverlay": "Wrapper"`, `excludesBaseTranslation:false`. A startup abort mentioning "Stale generated indirect dispatch" means the chain was not run in full.
2. `tools\run_game.ps1 -Seconds 60 -Name menuhook -Env "SIN_MENU_HOOK_LOG=1,SIN_OPEN_OPTIONS=1"`:
   expect `[hle] game-menu title-root: menu=0x... flags=0x8....... entered=1` when the title menu appears, then
   `game-menu: SIN_OPEN_OPTIONS=1, title menu reached ... -> overlay` and the overlay in the screenshot. (Proves the hook is called and the guest thread->UI path works.)
3. Real path: title, A on "Press", pick OPTIONS (3rd item). Expect `game-menu title: menu=0x... entered=1 overlayOpen=0` followed by `OPTIONS chosen in the title menu`, overlay visible, game input ignored; close overlay and the stock Options page is shown; B returns to the menu. Then 1P Start, play, `+` (pause), OPTIONS: expect the same with `pause`.
   Use `SIN_VREMOTE_SCRIPT` to drive A/D-pad if no pad.
4. Failure triage: (a) no `title-root` line => registration/dispatch problem (check native-index, `[hle]` log, native not winning); (b) `title-root` ok but no `title` line when OPTIONS is chosen => handler 0x8010BC48 is not the page (dump `mem1.bin` with `python tools/menu_objs.py` and read `+0x204` of live menu objects; it must equal `0x8010BC48`/`0x8010C1E4`); (c) overlay opens at menu creation but flicker/reopen => raise the gap or key on bit31 (`flags & 0x80000000`) only.

# Swap crash: `InvokeIndirectCpu: target 0x496a0100 not translated` (static analysis, 2026-11-20)

Artifact: `build/native/UserData/Logs/base_1795132856_pid29448` (SIN_AUTOSTAGE_SEQ=1,2,1). Static analysis only,
nothing was run. Tools: `tools/dis_dol.py`, `tools/callers.py`, `tools/xref_data.py`, `mem1.bin`/`mem1.bin.mem2` parsed in Python.

## 1. Bottom line

The crash is NOT in the module swap and NOT in the stage-flow state machine. The game is in the **GAME OVER scene**
(scene object `0x80AA3960`, label string `GAMEOVER` at +4, vptr 0x802FEF40) and its first state (state 0, +60 frames)
asks "is the game-over jingle ready?" for a sound-manager slot whose **handle object was already freed**. The freed 24-byte
block is now interior to a later big heap block, so `*(*h)` is garbage and the virtual call goes to `0x496A0100`.
It is a stale-handle read; the forced 4->5 change is at most an indirect factor (see section 5).

## 2. What the three functions are

| Function | What it is | Evidence |
|---|---|---|
| `fn_800F8014` | **GameOver scene update** (vtable at 0x802FEF98, ctor `fn_800F9B28`, factory ptr in table 0x802FEF48). States (byte +54): 0 wait 60 frames then `fn_8003B950(&slotvar)` ready check, if ready `fn_8003BB00` (start music); 1 fade; 2 wait; 3 wait A/Start (`GM+4+0x3C & 0x400`) or 240 frames then `fn_8003BCC4(slotvar,30)` (music fade-out); 4 fade then `fn_8003D35C(GM+96,0)` = change to scene 0 | `dis_dol 0x800F8014`; crash task at stack word +1C = `0x80AA4180`: state byte +54 = 0, u16 counter +56 = `0x3D` (61), i.e. exactly the first ready check. Scene mgr `GM+96 -> 0x80AA3960` holds `47414D45 4F564552` = "GAMEOVER" |
| `fn_8003B950(int* slot)` | `SoundMgr::IsSlotReady(slot)`: `mgr=*(GM+92)`, `entry = *(mgr+1004) + *slot*68`, `h = *(entry+44)`, `P = *h` ; `if (P) { vt=*P; call vt[+20]; }` | quoted: `8003B988 lwz r3,44(r3)` `8003B98C lwz r3,0(r3)` `8003B998 lwz r12,0(r3)` `8003B99C lwz r12,20(r12)` `8003B9A4 bctr`. No bounds, no loaded-check. (`fn_800F7A40` = `isNextMusicReady` guards slot<0 first; `fn_800F8014` does not) |
| `fn_800CA00C(+0x66C)` = `fn_800C99A0+0x66C` | **Irrelevant stale stack word**: the return address after `bl 8002462C` inside fn_800C99A0 (a UI/sfx call), left at sp+4 of an older frame. Not on the live call chain (the real chain is fn_800F8014 <- fn_8003CCA8 <- fn_8003CFFC <- ... <- main loop, see backchain) | backchain in `crash_missing_target.txt` |

Sound manager layout (nw4r::snd based, `SoundHeap::LoadState` = `80039B40(mgr, level)`; 0x8027ACA0): `entries = *(mgr+1004)`,
68 bytes each. Fields: +0 4-char id string, +40 state (3 = loaded, **5 = unloaded**), +44 `h` (24-byte block `new`'d from
heap 0 by `fn_8002C7BC`, see `0x8003A144..A160`), +48 data object, +52 list of owners, +64 vptr `0x802F5BBC`.
Load function `fn_80039FEC(mgr,id,slot,flags,rate)`: if slot>=0 it first **unloads** the entry (`8003A088 li r0,5; stw r0,40(entry)`,
frees `h`, frees +48) then allocates a fresh `h`. The unload code (`0x80038AE8..80038B48` and `0x8003A088..`) frees `h` but **never nulls entry+44**.
So by design a slot with state 5 has a stale `+44`; the game relies on never asking readiness of an unloaded slot.

The game-over jingle is **music id 1901, slot 5, flags 2, rate 0xBB80**: `fn_8003B7F4(1901,5,2,0xBB80)` (called from
`fn_800F6830` at `0x800F6A3C` only when a float timer converts to an int > 0 (`0x800F6A20 cmpwi r0,0; bgt` -> skipped otherwise)
and from `fn_800F823C`) and the return value is stored in the global slot variable `0x804B1CD0` (`0x800F6A40` / `0x800F827C`,
reset to -1 by `0x800F9DF0`). Slot 5 is only ever used for this jingle (stream `gover.brstm`).

## 3. What `0x80BC9940` is (memory evidence, `mem1.bin`)

* `slotvar (0x804B1CD0) = 5`, entry 5 at `0x8072CEC4`: `+40 = 5` (unloaded), `+44 = h = 0x80B6BA80`, `+48 = 0`.
  Entries 3 and 4 (also state 5) have `h` = 0x80B67D60 / 0x80B67E00, also inside the big block below with no heap header: same disease.
  Entries 0/1 are healthy (state 3): `h` = 0x808032E0 / 0x8076AD60 are real heap0 blocks and `*h` = `0x97991880` / `0x979907CC`,
  MEM2 sound objects whose words are `[0x8032BEA0 (vptr in DOL .data), 0, h (back pointer), ...]`. **That is what `*h` of a live slot must look like.**
* `h = 0x80B6BA80` has no ExpHeap header (`0x80B6BA70` is zero). It is interior to the heap0 block `0x80B60450`
  (size `0x34480`, an array of ~200 objects whose first word is `0x80BC9940`; `0x80B6BA80` is one of them). So the old 24-byte block was freed and the memory was
  reused later; `*h` now reads that unrelated object's first word.
* `0x80BC9940` itself lies at the tail of heap0 block `0x80B948F0` (size `0x35480`) = a **REFT effect file for stage 2**
  (`52454654 'REFT'`, `ff_s02`, `EFT_S02` strings). `0x80BC9940` is a 32x32 image header (`0x00200020`, size `0x400`, begin `0x80BC9980`, end `0x80BC9D80`).
  Its first word `0x80B94BAE` is not a vtable; `*(0x80B94BAE+20) = 0x496A0100`.
* Heap integrity: heap0 (`EXPH` at 0x804C68C0, range 0x804C6910..0x817F0520) block list is consistent (headers 'UD'/'FR' chain correctly). Nothing indicates an overflow/stomp.
  Everything that allocates/frees (`MEMAllocFromExpHeapEx 0x801D3660`, `MEMFreeToExpHeap 0x801D3710`) is **translated guest code**; grep of `runtime/src` finds no HLE of
  any MEM*/ExpHeap function. Allocation behaviour is the original's.

## 4. HLE overlap check (item 2)

* Module window `0x97DC0000..0x97FC0000` (MEM2) is above the game-visible MEM2 arena (`0x80003128` arena hi = `0x97BC0000`, FST region from there) and nowhere near heap0 (MEM1) or the sound heap
  (`0x9797xxxx..0x9799xxxx`). The HLE (`os_rso.cpp`) only memset/memcpy's the window. No overlap, so **HLE cannot make a heap0 block alias the window.**
* The stage RSO buffer lives in heap0 (`MEMFreeToExpHeap(heap0, rso)` in `fn_8001F25C`); the unlinked/freed buffers are returned through the game's own free. No HLE involvement.
* `sin_autostage.cpp` writes only F+54..57, GameData flags/bytes (GM+100.., GM+108/244) and never touches the heap.

## 5. Is the forced 4->5 change a plausible cause? (item 3)

`fn_800C2704` (F, jump table 0x802FD580): 0 wait, 1 `fn_8003AFDC(mgr,-1)` then `80039B40(mgr,3..0)` (roll the sound heap back), 2 wait, **3 load stage**
(`fn_800F9190 -> fn_8001F478`), **4 stage running** (poll the stage task at F+0x3C, create the 128-byte object, then advance),
**5 post-stage dispatch** (`80039B40(mgr,2)`, reset GameData+68.., then: GM flags bit31 -> state 10 (`8003D35C(GM+96,0)` = back to scene 0);
bit30 (`0x40000000`, "start stage from select") -> reload `F+55..57` from GameData next (`8008F0D0`) -> **state 3**; bit4 -> 6/7/9 result path).
Measured facts: in both swaps F stayed at 5 for ~100 s (stage 1: 69.45 s -> 183.05 s) although the 5-handler would exit to 3 in one tick if it ran.
So **F is not ticked while the stage scene / game-over scene is alive**; state 5 only executes once the stage's own scene ends. In the stage-1 swap that end was the player dying
(gover.brstm streamed at console line 2449), the game-over scene ran fine, then F 5->3. In the stage-2 run the same death happened (scene = GAMEOVER), but there is **no `gover.brstm`
read after t=193** (console shows only the first one), i.e. the jingle load (`fn_8003B7F4(1901,...)`) never ran for this death, `slotvar` was still the 5 from the first death, and slot 5 had
been unloaded in between (state 5, stale h). The original code would read the same stale slot, so the game assumes the load always happens first.

Verdict: forcing 4->5 is a legitimate way to *schedule* the swap (it sets exactly the fields the game's post-stage dispatch reads) and did not corrupt memory.
But the test only completes when the stage ends by itself, and the idle bot's stage end is "die -> GAME OVER scene", a path we have now seen crash.
I did not change the trigger: the clear/exit path (what sets GM flag 0x10/0x80000000 and creates the result scene) has not been decoded, and inventing flag writes would make the test less realistic, not more.

## 6. Ranked hypotheses

1. **(~60%) Game-over jingle (music 1901, slot 5) not loaded for this death, readiness check on a stale slot.** Evidence: scene = GAMEOVER, state 0 counter 61, slotvar=5, entry5 state 5 / data 0 / h dangling,
   `*h` not a MEM2 object (healthy entries 0/1 show the contract), no gover.brstm read in stage 2, `UnloadSlot` never clears +44. Open question: *why* the load was skipped. Either the death path in stage 2
   (boss / underwater) legitimately goes through a loader we do not execute, i.e. an **unexecuted/mistranslated branch or a missed indirect call** around `fn_800F6830` `0x800F6A00..6A3C` (condition: `fn_8002F8E8` result), or the load ran and
   failed (`fn_80039FEC` returns -1 when no free slot -> but slotvar would be -1, not 5).
2. **(~20%) Load did run in stage 2 but earlier/elsewhere and was then undone by an extra unload** (`80039B40(mgr,1..3)` rollback or `fn_800F823C`), e.g. by F's state-5 handler or an `80038A9x` unload running at the wrong time because our
   forced state 5 made F's cleanup (rollback + slot resets) run in a different order relative to the death. Would show as `entry.state` flipping 3 -> 5 in the trace below *after* the death began.
3. **(~10%) Genuine original-game latent bug exposed by memory reuse** (stale +44 is read by the game on hardware too, but there the freed 24-byte block still holds the old MEM2 pointer, so the call "works"; here the stage-2 REFT loader recycled it). Plausible only
   if hypothesis 1's skipped load is itself legitimate on hardware.
4. **(<5%) Host/HLE heap overlap** (section 4 rules the window, MEM natives and autostage writes out).
5. **(<5%) Heap stomp by translated code** (heap chain is consistent; the object at `0x80BC9940` is a normal REFT image header, not a stomp victim).

## 7. Cheapest confirming experiment

New env switch in `runtime/src/hle/os/sin_autostage.cpp` (the only runtime file touched): **`SIN_AUTOSTAGE_TRACE=1`**. A 50 ms host poll logs every change of
`scene vptr + label ("GAMEOVER"...) | slotvar 0x804B1CD0 | sound entry[slotvar] state/+44/*h/+48 | GM+100 flags`, and appends ` DANGLING` when slotvar names an unloaded entry whose `*h` is not a MEM2 object.
It writes nothing to the guest. Run the same `SIN_AUTOSTAGE_SEQ=1,2,1 SIN_AUTOSTAGE_TRACE=1` once (the crash reproduced after ~273 s; first swap leg alone is enough to see stage 1's death: gap 25 s default, use `SIN_AUTOSTAGE_GAP=10` as before).

Reading the log:
* `scene=... 'GAMEOVER'` appears together with `DANGLING` and **no earlier line with `slotvar=5 entry.state=3`** after t=193: hypothesis 1 confirmed (jingle never loaded); then look at why `fn_800F6830` skipped (see step 2).
* `entry.state` goes 3 then 5 *between* the death and the scene switch: hypothesis 2; the same log shows which scene/flags were live at that moment.
* If `*h` changes from a `0x97...` value to a MEM1 value while state stays 3: a real heap stomp (hypothesis 5).

Follow-up (only if hypothesis 1 holds), static first: print the guest function value in `fn_800F6830` at `0x800F6A1C` (`r0` after `fn_8002F8E8`) with a one-shot log in the translated shard or use `SIN_CALLTRACE` to check whether `fn_8003B7F4`/`fn_80039FEC` run at all during the stage-2 death; compare with the stage-1 death where they do. If they do not run in stage 2, find which of the game-over entry paths (`fn_800F823C`, its caller reached through the table written by the registration function at `0x8001D200..0x8001D398`) the stage-2 death uses and whether a vtable/indirect call on that path is silently skipped.

## 8. For the lead (verify steps)

1. Rebuild (your side), run `SIN_AUTOSTAGE=1 SIN_AUTOSTAGE_SEQ=1,2,1 SIN_AUTOSTAGE_GAP=10 SIN_AUTOSTAGE_TRACE=1`.
2. `grep autostage console.log | grep -E "scene=|DANGLING"`: confirm which of the three patterns of section 7 occurs and when relative to `gover.brstm`.
3. Optional sanity run with `SIN_AUTOSTAGE=2` only (no sequence): if stage 2's idle death also crashes in the GAME OVER scene, the swap and forcing are exonerated completely.

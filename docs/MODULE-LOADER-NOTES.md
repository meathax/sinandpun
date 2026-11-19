# Stage module loader: how the game really drives the RSO API (measured 2026-11-20)

All evidence is from `orig/R2VE01/sys/main.dol` disassembled directly (capstone) plus the generated shards.
Addresses are Sin R2VE01 guest addresses. Read-only research; only `runtime/src/hle/os/os_rso.cpp` was edited.

## 1. The RSO API at Sin addresses

| Address | Real function | Signature / behaviour (from the code) |
|---|---|---|
| 0x8020D618 | (unnamed) header fix-up `Fix(rso, bss, level)` | converts every header offset to an absolute pointer (+0xC section table, +0x10 name, +0x30/+0x38/+0x40/+0x48/+0x4C/+0x54 tables), section table entries `off+base` (a size!=0, off==0 entry is the bss: stores `bss` arg, sets byte +0x23), prolog/epilog/unresolved +0x24/+0x28/+0x2C = section base + offset, applies internal relocs (RSORelocate 0x8020E4A0), `if (level>=1) internalRelSize(+0x34)=0` |
| 0x8020D990 | RSOStaticLocateObject(rso) | same job for the static module (gs2.sel) |
| 0x8020DC9C | un-fix (inverse of D618) | subtracts `rso` back out of the relocation/section tables; called last by RSOUnLinkList |
| 0x8020E064 | RSOLink(a, b) | bind a's imports to b's exports (RSORelocate per import) |
| 0x8020E194 | RSOUnLink(a, b) | re-point a's imports of b to `a->unresolved` (+0x2C) |
| 0x8020E8BC | RSOListInit(staticRso) | `head=tail=0` (r13-0x50E0/-0x50DC), calls D990, makes it list element 0 |
| 0x8020E92C | static `LinkList(rso, bss, level)` (sdk_name_map "LinkList") | D618 fix-up (returns 0 on failure), RSOLink both ways against every list entry, append to list, `if (level>=2) { memset(bss,0,rso[0x1C]); rso[0x3C]=rso[0x50]=0 }`, return 1 |
| **0x8020EA08** | **RSOLinkListFixed(rso, bss, level)** | `cmpwi r5,2; ble 0x8020EA18` -> `b 0x8020E92C`; level > 2 -> return 0. Game uses level **1** |
| **0x8020EA20** | **RSOUnLinkList(rso)** (VERIFIED, was a guess) | `if (rso->next==0 && rso->prev==0) return 0`; unlink from list; `RSOUnLink(x,rso)`+`RSOUnLink(rso,x)` for all others; call 0x8020DC9C; return 1 |
| 0x8020EAD8 | `GetSectionOffset(rso, kind)` (game-only helper, plain translated code) | kind 1 -> `[rso+0x30]` (internal-reloc table file offset), kind 3 -> `[rso+0x38]`, 0/2 -> 0. Gives the resident (reloc-free) size |

There is no RSOLink/RSOUnLinkList *name* collision: sdk_name_map's `RSOGetExport` / `RSOGetImport` at 0x8020ED38 are
**bogus** (0x8020ED30/ED50 are tiny `ut::List_GetNext/GetPrev`-style helpers called from 100+ game sites,
0x8020EB1C is List_Init). `rso.a` ends before 0x8020EB1C. **Outside the stage loader nobody in main.dol calls any
RSO function** (bl scan of the whole text: callers of 0x8020D5F0..0x8020EB1B are exactly 0x8001F324 (ListInit),
0x8001F3B0 (GetSectionOffset), 0x8001F4FC (UnLinkList), 0x8001F560 (LinkListFixed)).

### Loader state machine: fn_8001F478(stage, a, b) driving fn_8001F25C(task)
Globals (r13 SDA): `-0x5D98` byte = resident stage id, `-0x5D94` = loader task (0x20 bytes, new'd per request),
`-0x5D9C` = resident module buffer (RSO header address), `-0x5DA0` = gs2.sel buffer (loaded once).
Task fields: +0 DVD handle, +4 out buffer, +0x14 loaded file size, +0x18 stage byte, +0x1A state, +0x1C resident size.

fn_8001F25C states: 0 load `gs2.sel` (path 0x802D52F3) once; 1 poll, `RSOListInit(buf)`, `-0x5DA0=buf`; **0x10** async-load
`/stgNN.rso` (path table 0x802F5260, index = stage byte); **0x11** when loaded: `n = align32(GetSectionOffset(rso,1))`,
`task+0x1C = n`, `size = max(align32(filesize), n + rso[0x1C] /*bssSize*/)`, `MEMFreeToExpHeap(heap0, rso)` and **load the
same file again** into a fresh allocation of `size` bytes (so the buffer is `[file image | zero-able bss tail]`); **0x12**
poll, `-0x5D9C = task+4`, return 1.

fn_8001F478 sequence (quoted):
```
8001F494 lbz r0,-0x5d98(r13) ; cmplw r0,r3 ; beq 0x8001f5c0     ; same stage as resident -> reuse (below)
8001F4A0 lwz r0,-0x5d94(r13) ; cmpwi r0,0 ; bne 0x8001f518        ; request already in flight
 ...new task...  lwz r29,-0x5d9c(r13) ; beq skip                   ; there is a resident module:
8001F4EC lwz r12,0x28(r29) ; mtctr r12 ; bctrl                    ;   1. EPILOG via header +0x28 (absolute ptr)
8001F4F8 mr r3,r29 ; bl 0x8020ea20                                ;   2. RSOUnLinkList(old)
8001F508 mr r4,r29 ; bl MEMFreeToExpHeap                          ;   3. free old buffer
8001F510 stw r31,-0x5d9c(r13)                                     ;   4. resident = NULL
... (poll 8001F25C until 1)
8001F53C lwz r0,0x1c(r3)  ; (rso bssSize) beq -> r29=0
8001F54C lwz r4,-0x5d94(r13); lwz r0,0x1c(r4); add r29,r3,r0     ; bss = buf + task+0x1C
8001F55C li r5,1 ; bl 0x8020ea08                                  ; RSOLinkListFixed(buf, bss, 1)
8001F570 lwz r5,0x1c(r5) ; mr r3,r29 ; li r4,0 ; bl memset        ; game clears bss itself (level 1 does not)
8001F588 __dl(task) ; -0x5d94 = 0
8001F5A4 lwz r12,0x24(r6) ; li r6,3 ; bctrl  (r3..r5 = stage,a,b)  ; PROLOG(stage,a,b,3) via header +0x24
8001F5B4 stb r26,-0x5d98(r13)
8001F5C0 same stage: lwz r12,0x24(r6) ; li r6,2 ; bctrl            ; PROLOG(stage,a,b,2), no reload, no unlink
```
Module `_prolog(a,b,c,flags)` (stg00 @0x97E16C84): `flags&1` -> walk the NULL-terminated `.ctors` array and call each;
`flags&2` -> `initStgNN(a,b,c)`. `_epilog` (@0x97E16CFC): walk `.dtors` (= `__destroy_global_chain`,
`__fini_cpp_exceptions` -> `__unregister_fragment`). So fresh load = mode 3 (ctors + init), re-entry of same stage =
mode 2 (init only, **no** ctors, bss kept).

### What this means for stage A -> B -> A and the window
* The old module is torn down when the **next different stage is requested** (epilog -> unlink -> free), *before* the new
  file finishes loading. Between unlink and link no module code may run (matches `g_active==nullptr`). `-0x5D98`
  keeps the old stage id until the new prolog finishes (a re-request of the old stage in that gap would call a NULL
  prolog on hardware too).
* A -> A (same id) never reaches the HLE: no relink, ctors not re-run, window and bss must stay untouched. Current HLE
  satisfies this (it only acts on link/unlink).
* A -> B -> A: every re-link goes through the real file read (header is **raw/offset form** again) -> HLE memsets the whole
  window, copies the pristine image (image contains the zeroed bss, see below), so `.data/.ctors` state, the
  `__init_cpp_exceptions` fragment slot (-2 sentinel in `.data`) and all bss start fresh -> ctors run again correctly.
  No stale image / stale code set / stale header problem exists as long as link->`memset+memcpy` and unlink->`g_active=null`.
* Epilog is called **by the game** through header +0x28, *before* RSOUnLinkList. Therefore HLE must (a) write absolute
  prolog/epilog into the buffer header at link (done), (b) keep the code set active until RSOUnLinkList (done). The HLE
  must NOT call the epilog itself (would double-run dtors / double `__unregister_fragment`).
* **Header state.** HLE leaves the buffer header in raw file form (offsets) except +0x24/+0x28/+0x2C. The real linker would
  make it absolute and (level 1) clear +0x34. Nothing reads these afterwards except RSOUnLinkList's own un-fix, which
  is HLE'd. Safe. The module list (`-0x50E0/-0x50DC`, rso+0/+4) is also not maintained by HLE; nobody else reads it.
* **Name used to select the module**: `rso+nameOffset` is a raw file offset at link time (valid because D618 has not run).
  Names are `C:\dev3\gs2\src\stgNN\stgNN.plf`; `rfind("stg")` hits the file part. OK for all 8.

### bss: window vs game buffer — they disagree, harmlessly
* rso_link.py lays out file sections first, then bss sections, and **the `.bin` includes the zeroed bss** (e.g. stg00 sections
  7/8, 40 264 + 32 708 bytes, inside `end-base` = 0x70670). Plus the far-jump stubs. HLE additionally `memset`s the
  whole 2 MB window before the copy, so bss is zero on every link.
* The game's own bss area (`buf + task+0x1C`, `bssSize` bytes, header +0x1C) lives in the MEM1 heap buffer and is cleared by
  fn_8001F478 (not by RSO at level 1). Module code **never** touches it: relocations resolved by rso_link point at the window
  bss section, and no other game code gets the heap bss address. So the two copies never need to agree. The only cost is
  wasted heap (identical to hardware allocation sizes, so heap behaviour/fragmentation matches the original).
* Header +0x1C (`bssSize`) must remain readable raw: the game reads it (8001F53C/F570). HLE does not modify it. OK.
* Image size vs window: largest is stg06 1 752 480 bytes < 0x200000. OK.

## 2. Does anything read export/import/section tables at runtime?
No.
* No main.dol code calls RSOGetExport/RSOGetImport/RSOStaticLocateObject post-boot (only the loader calls RSO functions, see
  scan above; the "RSOGetExport/Import" names at 0x8020ED38 are mislabels of `ut::List` helpers — delete them from
  tools/sdk_name_map.txt so nobody HLEs them).
* Module code: none of the 8 .rso imports contain any RSO/OSLink/Export/Import symbol (checked all 8 import lists); modules
  never read their own header or section table (they have no pointer to it; the header exists only in the game's heap buffer).
* All imports resolved (0 unresolved, all 8). The sole dynamic consumer of "section 2" is `__init_cpp_exceptions`
  (below).
* gs2.sel buffer (`-0x5DA0`) is only touched by RSOListInit/D990; the recomp never needs it at runtime (imports are bound by
  rso_link.py at build time).

## 3. ctors/dtors, sinit, relocation
* `.ctors` / `.dtors` arrays (section 4 / 5 in the .rso; image offsets e.g. stg00 0x97E16DA0 (27 entries) / 0x97E16E20 (2))
  are applied by rso_link.py's internal R_PPC_ADDR32 relocs: all entries in all 8 modules lie in the window and are
  NULL-terminated. Prolog/epilog of every module are emitted translated functions (checked 8/8).
* dtors list is always `[__destroy_global_chain @0x97DC001C, __fini_cpp_exceptions @0x97DC016C]` (stg03: 0x97DC0128/0x97DC0278).
* `__init_cpp_exceptions` (ctor) does `__register_fragment(_eti_init_info, r2)` -> main.dol 0x80142328 stores the two
  words in a 32-slot table at 0x803x1F28 and returns an id kept in `.data` (-2 = none). `__fini` unregisters. rso_link.py
  relocates the 4 relocs against the *empty* section 2 (`missing_section_relocs`) to the poison `0xDEAD0000+0x1d64`. On
  hardware the value would be `0 + 0x1d64` (also garbage), and it is only dereferenced by `__find_exception_addresses`
  when a C++ exception is thrown. Acceptable; if you want a belt-and-braces: point those 4 relocs at a zeroed 16-byte
  area at the image end (size field `[+0xC]==0` makes `__find_exception_addresses` return 0).

### REAL BUG FOUND: 7 static-init functions are not translated
tr_stg0N logs: `skipped map entry 0x97DF634C: Disassembly budget exhausted ... after 8192 reachable instructions
(maxInstructions=8192, maxBytes=0x10000)`. These are huge `__sinit_*` functions (stack frames 0x3470..0x7FF0) that are
**entries of the .ctors array**, so `_prolog(...,3)` will `bctrl` into them and `ModuleOverlay::TryInvoke` will miss
(no `func_*.cpp`):

| module | ctor entry (missing) |
|---|---|
| stg01 | 0x97DF634C |
| stg02 | 0x97DFC4DC, 0x97E7AD78 |
| stg03 | 0x97E2A6A0, 0x97E43780 |
| stg04 | 0x97E6B8B4 |
| stg06 | 0x97EF03B4 |

(stg00, 05, 07 are complete.) `tools/module_coverage.py` lists them as `interior ok src=data,prologue` — it classes
them as covered-looking "interior"; they are not. Fix is in the translator/config (raise the 8192-instruction / 0x10000-byte
limit used by `PpcDisassembler.DisassembleFunction` for module entries — `translator/src/Translator.Cli/Program.cs`
`TranslationOptions.Default.MaxInstructions/MaxBytes` and `InferredGuestFunctionAbiProvider.cs:380`), then regenerate
the 5 modules. Until then stage 01..04, 06 will abort at stage load (stg00/05/07 will not). The same failure hits other
"interior ok" entries (30 in stg01 alone, mostly `lisaddi`, i.e. vtable/function-pointer targets): re-run
`python tools/module_coverage.py --list` after raising the budget.

## 4. Changes made / still needed in os_rso.cpp
Made (confirmed by the disassembly above):
1. `RSOUnLinkList` address 0x8020EA20 is correct — documented in the header comment (no change in address).
2. `RSOLinkListFixed_HLE`: `level > 2 -> return 0` exactly like 0x8020EA0C; comment that the heap `bss` arg is deliberately unused.
3. Module code set is looked up by **name** (`stgNN`), not by table index (the table is whatever module_shards.py was run with).
4. `RSOUnLinkList_HLE` returns 0 when nothing was linked (real function returns 0 if not in list), else 1.

Not changed / recommendations:
* Do not call the epilog from HLE; the game does (8001F4EC). Do not invalidate anything on A->A.
* Optional: cache the stage `.bin` (re-read from disk each load: 0.5-1.7 MB; fine).
* Optional hardening: if link fails (returns 0) the game still calls prolog at header +0x24, which then holds a raw file
  offset and crashes obscurely; consider an explicit fatal log there.
* sdk_name_map.txt: remove `RSOGetExport`/`RSOGetImport` at 0x8020ED38 (interior of List helper), add
  `RSOUnLinkList 8020EA20`, `RSOUnLink 8020E194` (exists), `LinkList` stays 0x8020E92C.
* Fix the 7 untranslated ctor targets (section 3) — this is the blocker, not the HLE.

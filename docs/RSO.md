# Stage modules (RSO): findings and design

## Facts (measured 2026-11-17)
- Stage code lives in `files/stg00..07.rso` (Nintendo RSO format: `_prolog`, `_epilog` and
  `_unresolved` entry points plus import/export tables). `dtk rso info` parses them; the
  exports are mangled CodeWarrior C++ names, so classes are recoverable (`S01_EnmSoldier*`, ...).
- `main.dol` has no OSLink/RSOLink symbols (`gs2.sel` names only `RSOStaticLocateObject` at
  0x8020D990), so the game uses its own module loader.
- `main.dol` holds the module path table at `.data` 0x802F5260: 8 pointers to `/stg00.rso` ..
  `/stg07.rso` (strings at 0x802D5280, 12-byte stride; the strings sit in .rodata).
  A scan of `.text` for lis/addi references to the table (0x802F5260) found none, so it is
  reached some other way (indexed from a struct base or through a different register
  pattern). **The loader function is not located yet.**
- `dtk dol split` on `main.dol` alone works (11,089 functions, 2,178 named from SEL).
  With the 8 RSOs added, dtk 1.9.1 fails: `Failed to locate section 13 for
  .dwarf_pubnames.__vt__8S00_Bg00` (debug-info sections in the RSO). The modules are
  therefore currently disabled in `decomp/config/R2VE01/config.yml`.

## Plan
1. **Find the loader** at runtime or by static trace: xref the table, then the file read
   and relocation code (likely something calling DVD read then applying relocations, or
   a `_prolog` call). Ghidra with the GameCube loader helps here; so does a Dolphin trace.
2. **Fixed load addresses.** The recompiler needs module code at stable guest addresses.
   Reserve a guest window per module (for example above MEM1's heap, or in MEM2) and make
   the HLE loader place stg0N there. Only one stage module is resident at a time in the
   original, so all 8 can share one window if we translate each against that same base
   and switch generated-code sets on load. Sharing a window is cheaper; separate windows
   avoid any aliasing ambiguity.
3. **Translate each RSO.** Extend the forked translator (`translator/`, GPL-3, detached from
   the MKWii Launcher project) with an RSO loader that resolves imports against
   `main.dol`/`gs2.sel` symbols and applies relocations at the chosen base, then emits
   per-module C++ (modules have ~3k-6.7k functions each). The existing REL support is the
   template.
4. **Register generated code** per module and dispatch indirect calls (virtual tables) by
   guest address as the main-DOL path already does.

## Open questions
- Does any stage load a second module while another is resident (the stage table suggests
  not)?
- Are the RSO relocations plain `R_PPC_*` against the DOL with no dynamic symbol lookup at
  runtime beyond `RSOStaticLocateObject`?

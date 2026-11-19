# Status (2026-11-19, evening)

## LIVE: the recomp RUNS (exit 0, sustained loop)

The boot loop became a RUNNING loop: the game executes its full init chain
(DOL -> OSInit -> main -> KPAD/G3D/GX/AX/SND -> aurora GX+audio HLE ->
engine periodic alarm -> FIRST GUEST THREAD CONTEXT SWITCH to the worker
entry 0x80280CC0) and then ticks its scheduler/decrementer loop for the
full 120 s observation window, exiting cleanly via window close (code 0).
No crash artifacts for the run.

Current investigation: the game ticks but has not engaged the render path.
Capped alarm-due diagnostics added to os_alarm.cpp to determine whether the
engine's periodic alarm reaches its fire time (guest/host time base) or
whether a thread-queue waker is missing.

## Boot-blocker ledger (all fixed, in order, evidence-backed)
VI TV-mode MMIO -> VIGetTvFormat/VIGetScanMode natives; OSInit assert complex
-> __OSInIPL planted from OSGetResetCode native; SYSCONF synthesized; OSBootInfo
header + retail console type; GX fatal stubs eliminated (205); WPADInit boundary
HLE; GXInit/PADInit/PADRead restored at SDK-verified addresses; alarm HLE
retarget (queue 0x804B2220, __OSTimeToSystemTime 0x801B0D60, InsertAlarm
0x801A6CF0); scheduler OSSaveContext 0x801A8800.

## Static evidence (committed)
- main.dol translates: 11,169 functions, 0 unsupported insns (generated/)
- stg00-07 linked flat @ 0x80800000 (tools/rso_link.py), 0 unresolved imports
- SDK naming: 2,451 functions + 363 data + 905 globals + 36 layout via the
  final Wii SDK archives (0x4302_145) -> tools/sdk_*.txt maps
- Translator module-image support in progress (FlatModuleImage + builder)
- Test baseline: 767/0/8 green

## Next
- Resolve the render-path stall (alarm diagnostics in flight)
- Module output-space design (8 modules share 0x80800000: per-module roots
  + runtime code-set switching), RSO loader xref
- Dual-analog aim (THE feature) at the host-owned input boundary
- Playthrough validation vs original

## Known gaps
- Dormant MKW_GUEST_FUNC donor sites (ax_effects, network_config, os_init
  801A961C/8055531C) - retarget as their subsystems activate
- 164 GADDRs pending (tools/retarget_pending_gaddr.txt)
- __OSDBJump bla 0x60 gap; 4 extabindex relocs per stg module (benign)

The retargeted exe EXECUTES translated Sin code. Confirmed working:
embedded DOL sections, SDA bases (r13/r2), OSReport HLE printing the game's
full Revolution OS banner chain (Kernel/Console/NAND/NWC24, MEM1 Arena
0x804ba960 - matches the Facts table), OSInit -> OSInterruptInit/EXIInit/
SIInit natives, OS message natives (0x801AD2E0/340/410/4F0).

Boot loop, fixed in order:
1. VI MMIO read 0xCC00206C (VIGetTvFormat, Feb-2009 SDK reads the TV-mode
   register directly) -> native HLE at 0x801BC440; SISetSamplingRate 0x801B8830.
2. OSFatal("Failed to register network shutdown function") ->
   OSRegisterShutdownFunction native (success-stub) at 0x801AE130.

Each crash artifact names its own fix (symbolized backchain + mem1.bin
strings); iteration cost ~3 minutes. Run:
  build/native/WiiCompiled.exe with UserData/Config.toml [paths] dvd_root =
  'D:/Arcade/AI/aCORES/Sin/orig/R2VE01' (portable.txt staged).

## Static evidence (all committed)
- main.dol translates: 11,169 functions, 0 unsupported insns (generated/)
- stg00-07 linked flat @ 0x80800000 (tools/rso_link.py), 0 unresolved imports
- SDK naming: 2,451 functions + 363 data + 905 globals + 36 layout via the
  final Wii SDK archives (0x4302_145) -> tools/sdk_*.txt maps
- Natives retargeted: 264 registrations + 29 globals rewired, 288 disabled
  (SIN_DISABLED_*), raw registrations fixed via tools/sin_rawregs.py
- Test baseline: 767/0/8 green

## Next
- Continue boot loop to first pixel (title)
- Module translation support (Program.cs guard), RSO loader placement
- Dual-analog aim (THE feature), controller setup UI, playthrough validation

## Known gaps
- GXSetVtxDesc case-label sub-natives, KPAD/THPPlayer/AX internals unresolved
- DVD queue/context bases + 164 GADDRs pending (tools/retarget_pending_gaddr.txt)
- __OSDBJump bla 0x60 gap (debugger stub)
- 4 missing extabindex relocs per stg module (CW exception tables, benign)

| Step | State |
|---|---|
| Toolchain staged (docs/TOOLING.md) | done |
| Disc extracted, hashes recorded (docs/DISC.md) | done |
| dtk analysis of main.dol: `decomp/config/R2VE01/` (11,089 functions) | done |
| Function map `projects/r2ve01/MAP.txt` (11,157 entries, save/rest thunks normalised) | done |
| Manifest `projects/r2ve01/recomp.yml` (SDA r13=0x804B7700, r2=0x804BA920, decoded from `__init_registers`) | done |
| Translator fork `translator/` translating main.dol: 11,169 functions, 27 s, 0 unsupported instructions, 101 MB C++ (`build/generated/functions/`, 11,169 .cpp + `RuntimeConfig.h`) | done |
| Out-of-RAM `bla 0x60` in `__OSDBJump` skipped (translator patched); the emitted call still has no target | known gap |
| RSO static link `tools/rso_link.py`: stg00..07 linked flat at 0x80800000 (`build/modules/`), 0 unresolved imports each, 4 missing extabindex relocs each (CW exception tables, benign for recomp) | done |
| Translator manifest `inputs.module` (validated, sha256-pinned; CLI blocks output commands until pipelines consume modules) | done |
| Default translator test baseline: 767 passed / 0 failed / 8 optional-integration skips | done |
| Per-module translation (module image into guest space, per-module discovery/dispatch) | not started |
| Game's RSO loader located (xref of the 0x802F5260 path table / `RSOStaticLocateObject` 0x8020D990) | not started |
| `generate-data-init` .data/.rodata/.sdata initializer for main.dol | not done (RuntimeConfig.h only) |
| `emit-build-shards` + first C++ compile of the generated tree | not tried |
| Runtime (HLE OS, GX via aurora, VI/AX/WPAD/DVD, Sin main wiring) | not started |
| Features (uncapped fps, high internal res, dual-analog aim) | not started |

Regenerate: `dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release` then
`dotnet translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 8`
(output in `build/generated/`, untracked).

Regenerate modules: `python tools/rso_link.py orig/R2VE01/files/stg0N.rso orig/R2VE01/sys/main.dol orig/R2VE01/files/gs2.sel 80800000 build/modules/stg0N` for N in 0..7.

## Critical path to first boot

1. `generate-data-init` + `emit-build-shards`, then attempt the first clang compile of the
   11,169-function tree against a minimal runtime stub. Never tried; codegen compile errors
   surface here.
2. Module translation support in the translator (drop the Program.cs module guard once the
   image builder loads `inputs.module` at its base and per-module maps seed discovery;
   per-module code registration + guest-address dispatch for vtables, template:
   Wiicompiled `recomp_mod_loader.cpp` / DolRecomp REL folders).
3. Runtime: fork `reference/Wiicompiled/runtime` (GPL-3, same license), strip MKWii
   `game/`/`product/` specifics, wire a Sin `main.cpp`, DVD HLE over `orig/R2VE01/files`
   (U8 archives, stg RSO staging), OS/GX/VI/AX/WPAD HLE per `reference/aurora` and
   BlackKnight design notes (reference only, GPL-2-incompatible in one binary).
4. Locate the game's RSO loader to place/resolve modules at the 0x80800000 window (or HLE it;
   docs/RSO.md plan step 1).

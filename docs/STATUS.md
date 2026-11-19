# Status (2026-11-19)

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

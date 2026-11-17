# Status (2026-11-17)

| Step | State |
|---|---|
| Toolchain staged (docs/TOOLING.md) | done |
| Disc extracted, hashes recorded (docs/DISC.md) | done |
| dtk analysis of main.dol: `decomp/config/R2VE01/` (11,089 functions) | done |
| Function map `projects/r2ve01/MAP.txt` (11,157 entries, save/rest thunks normalised) | done |
| Manifest `projects/r2ve01/recomp.yml` (SDA r13=0x804B7700, r2=0x804BA920, decoded from `__init_registers`) | done |
| Translator fork `translator/` translating main.dol: 11,169 functions, 27 s, 0 unsupported instructions, 101 MB C++ | done |
| Out-of-RAM `bla 0x60` in `__OSDBJump` skipped (translator patched); the emitted call still has no target | known gap |
| RSO loader and per-module translation (docs/RSO.md) | not started |
| Runtime (HLE OS, GX via aurora, VI/AX/WPAD/DVD) | not started |
| Compiling the generated C++ | not tried |

Regenerate: `dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release` then
`dotnet translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 8`
(output in `build/generated/`, untracked).

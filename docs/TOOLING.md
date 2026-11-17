# Sin & Punishment: Star Successor PC recomp: tooling

Staged 2026-11-17. No game data is in this tree. Put your own disc dump in `orig/`; see `orig/README.md`.

## Strategy

Use static recompilation first, then decompile only what the features need.

1. **Get it running.** Recompile `main.dol` (plus any RELs) to C++ and link it against an
   aurora-based runtime. WiiCompiled (Mario Kart Wii) has already built this whole pipeline,
   including the three headline features: interpolated uncapped framerate, high internal
   resolution and aurora GX.
2. **Understand the game code.** Use dtk, Ghidra, m2c and objdiff to identify the frame loop,
   camera, HUD and 2D draw paths, and input/KPAD consumers. Feature work hooks these by symbol,
   so a full matching decomp is not required.
3. **Matching decomp (optional, long term).** `decomp/` is a fresh dtk-template, ready if we
   want byte-matching C/C++ for whole subsystems later.

## Staged contents

| Path | What | Version | Why |
|---|---|---|---|
| `tools/bin/dtk.exe` | decomp-toolkit | v1.9.1 | DOL/REL analysis, splitting, symbol/ELF generation, RARC/U8/Yaz0 tools |
| `tools/bin/objdiff.exe`, `objdiff-cli.exe` | objdiff | v3.8.3 | Function diffing (decomp matching, recomp checks) |
| `tools/bin/nodtool.exe` | nod | v2.0.0-alpha.12 | Extract or convert ISO/RVZ/WBFS/WIA; verify the dump |
| `tools/ghidra-ext/GameCubeLoader-*.zip` | Ghidra GameCube/Wii loader | 1.3.1 (built for Ghidra 12.1) | Loads DOL/REL/RSO with the Gekko/Broadway paired-single language |
| `tools/m2c/` | m2c decompiler | c632946 | PPC asm to C first draft for decomp |
| `decomp/` | dtk-template | 8a90f3c | Skeleton for the matching-decomp project (configure.py, ninja, auto-fetches the MWCC compilers) |
| `reference/Wiicompiled/` | Mario Kart Wii static recomp (GPL-3.0) | 03151e0 | **Primary base.** .NET translator (DOL to IR/SSA to C++), runtime, aurora fork with interpolation and high-res |
| `reference/Wiicompiled/aurora-main/` | aurora fork (submodule) | pinned | GX/VI/PAD/DVD on SDL3 + Dawn (D3D12/Vulkan/Metal) |
| `reference/aurora/` | upstream aurora (MIT) | f77492f | Upstream for diffing the fork |
| `reference/BlackKnightRecomp/` | Wii recomp runtime (GPL-2.0) | 497c652 | Shared runtime: WPAD/KPAD HLE that synthesizes Wii Remote samples, AX HLE audio, Revolution OS on fibers |
| `reference/SecretRingsRecomp/` | BlackKnight sister port (GPL-2.0) | 55cff5b | Shows how to retarget the same runtime to another SDK version |
| `reference/DolRecomp/` | DolRecomp (GPL-3.0) | 02e27ac | Alternate recompiler (C/LLVM backends, REL folders) |
| `reference/powerpc-recomp-core/` | DolRecomp core extraction (GPL-3.0) | 4fe554e | Clean decode/semantics/emit library |

All binaries match their GitHub release SHA-256 digests (verified 2026-11-17):

```
1782c628bbd13dc4ecd831ab33d96d5c818e7331942a89a7d344bfd148c78f6a  dtk.exe
2c1e8115685024b256caa5d6c2179a30a8850a5a9d99ee11843b5869b516874c  objdiff.exe
dd12dfb5e9e94a7f6c949a37e1815963cf2cffe4c87b4999b7ebc8c15b398a1f  objdiff-cli.exe
f65de62c44f0ad5d179eff85bd1e92099713ffacbf717f651fb761129a25c7bc  nodtool.exe
9892f28fc1e7f19bb3fcec7cfb4ad2ab7504503b571d93de8b142a6f7bec8ed0  GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip
```

Already on this machine and not duplicated here: Ghidra 12.2 at `D:/Arcade/AI/Decomps/apps/ghidra`
(the loader zip targets 12.1, so it may need rebuilding from source for 12.2), CMake, Ninja,
clang 22 (x86_64-w64-windows-gnu) and Python.

## Gaps and decisions

- **Disc dump required.** No image was found on D:. Supply R2VE01 (US), R2VP01 (EU) or R2VJ01 (JP).
  Pick one revision and pin its SHA-256.
- **.NET 8 SDK.** `dotnet` is installed but `--list-sdks` returned nothing. The WiiCompiled
  translator needs the SDK.
- **License.** Reusing WiiCompiled or DolRecomp code makes the project GPL-3.0. BlackKnight is
  GPL-2.0-only, which is incompatible with GPL-3.0 code in one binary. Use BlackKnight as a
  design reference only, or relicense with its author's consent.
- **Dolphin** is not staged. Install it yourself for reference captures and debugging, including
  the signature DB used for SDK symbol naming.
- **XenonRecomp** was rejected: it targets Xbox 360 and has no Gekko paired-single or quantized
  load/store support.
- **No prior SnP:SS decomp or recomp** turned up in the searches.

## Feature notes, for planning

- **Uncapped framerate.** The game logic almost certainly ticks on the VI retrace. Keep logic at
  60 Hz and interpolate rendering, as WiiCompiled does. Don't run the logic faster.
- **High resolution with nearest-filtered HUD.** Find the 2D/ortho HUD draw path (likely NW4R
  layout or game 2D). Force point sampling and integer scaling for those texture objects in the
  aurora GX layer, and keep 3D at high internal resolution.
- **Dual-analog aiming.** SnP:SS aims with the Wii Remote IR pointer. Synthesize IR pointer
  samples from the right stick at the WPAD/KPAD layer (the BlackKnight technique), with
  deadzone, sensitivity and response-curve settings, so the game sees an absolute pointer. Map
  the left stick to Nunchuk movement. Native Classic/GameCube controller paths: verify against
  the disc before relying on them.

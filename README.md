# Sin & Punishment: Star Successor - static recompilation (PC port)

A 1:1 static recompilation of *Sin & Punishment: Star Successor* (Wii, `R2VE01`) to native x86-64. The game's PowerPC code is
translated to C++ ahead of time; the Wii's OS, GX graphics, audio, input and storage are provided by a host runtime. The one
deliberate change from the original is input: a virtual Wii Remote is driven by a normal gamepad, keyboard and mouse
(right stick or mouse aims, left stick moves), with a full controller setup menu.

**No game data is included.** You need your own legally dumped disc. The code is GPL-3 (see `LICENSE`); the translator and
runtime were forked from the Wiicompiled project, from which all Mario Kart specific code has been removed.

## Status

See `docs/COMPLETION-PLAN.md` for the full completion workflow, `docs/STATUS.md` for verified evidence, and
`docs/REMAINING.md` for the one canonical remaining-task list. The game
boots, all eight stage modules load and render in isolated runs, audio produces mixed PCM, save files are created, and
the game-styled options overlay opens from both the title and pause OPTIONS pages, and a deterministic A->B->A module
swap now passes. A full playthrough, save/quit/reload, physical-device input and persistence, audible output, and
performance/fidelity validation remain open.

## Layout

| Path | What |
|---|---|
| `translator/` | .NET 8 PowerPC-to-C++ translator and its tests (`dotnet test translator`) |
| `projects/r2ve01/` | translation manifests: `recomp.yml` (main DOL) and `stg00..07.yml` (stage modules) |
| `runtime/` | host runtime (OS, GX/aurora graphics, audio, input, storage HLE), settings overlay |
| `aurora-main/` | rendering backend |
| `tools/` | analysis and run helpers (`run_game.ps1`, `run_shots.ps1`, `clean_build.ps1`, ...) |
| `docs/` | status, plans, reverse-engineering notes |
| `orig/`, `generated/`, `build/` | local only: your disc files, translator output, build output (not committed) |

## Build (Windows, clang + Ninja toolchain)

Prerequisites: the .NET 8 SDK, CMake and a clang/Ninja toolchain (see `tools/clean_build.ps1` for the exact toolchain
paths used here), and your game files extracted to `orig/R2VE01` (`sys/main.dol` plus `files/`).

```powershell
cd translator ; dotnet build -c Release ; cd ..
$t = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
dotnet $t emit-native-index --project projects/r2ve01/recomp.yml --out runtime/native-index.json
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 12 --output-metadata generated/base_translation_output.json
dotnet $t generate-data-init --project projects/r2ve01/recomp.yml
dotnet $t emit-build-shards --project projects/r2ve01/recomp.yml
powershell -File tools\clean_build.ps1 -Keep      # clean local configure + runtime build into build\clean
```

Stage modules (`stg00..07`) are pre-linked and translated separately; the commands are in `docs/STATUS.md` ("Build").
Run the translator tests with `dotnet test translator`. The current verified checkpoint includes a successful hardened
local clean runtime build. This is workstation-local evidence, not a cross-machine reproducibility claim.

## Running

1. Put the extracted disc root in `UserData/Config.toml` next to the executable:
   ```toml
   [paths]
   dvd_root = 'D:/path/to/R2VE01'   # the folder that contains sys/ and files/
   ```
2. Start `WiiCompiled.exe`. The first launch creates the save area under `UserData/NAND`.

## Controls

The default scheme (`[input] scheme = "pointer"`) presents a virtual Wii Remote + Nunchuk: the right stick or mouse moves the
pointer, the left stick is the Nunchuk stick. A Wii Classic Controller emulation (`"classic"`) exists but is **experimental**:
in testing the game did not accept its A button on the file-select screen, so it is not the default. Everything is remappable in the in-game options menu (Controls tab) for gamepad,
keyboard and mouse. Defaults on an Xbox-style pad: left stick move, right stick aim, RB shoot/sword, RT lock-on,
LB jump, LT evade, Start pause. The mouse can drive the pointer directly (absolute or relative mode).

## In-game options menu

Choose **OPTIONS** on the title menu or on the pause menu: the recomp settings (display, graphics, audio, controls, system)
open in a panel styled like the game. `F10`/`F1` and `Back+Start` remain as a developer fallback. Settings are stored in
`UserData/Config.toml`; display changes ask you to keep them (auto-revert after 10 seconds).

## Debug switches

`SIN_CALLTRACE`, `SIN_WATCHDOG_SECS`, `SIN_AUDIO_LOG`, `SIN_GX_COPYLOG`, `SIN_AUTOSTAGE`/`SIN_AUTOSTAGE_SEQ`,
`SIN_VREMOTE_SCRIPT` (scripted input), `SIN_MENU_HOOK_LOG` and more are listed in `docs/STATUS.md`.

## Known issues

- Save creation is proven, but save -> quit -> reload has not been completed.
- Repeated A -> B -> A module swapping is proven in an automated QA run; a full gameplay progression run remains open.
- A full playthrough, including bosses, cutscenes, continue/game-over, and credits, has not been completed.
- Audio produces non-silent PCM, but audibility on a real output device is not verified.
- Physical-device input, including Classic Controller acceptance, mouse aiming, remapping, and config persistence across a
  restart, remains unverified. The options overlay itself is verified from the title and pause menus.
- A dedicated performance and visual-fidelity pass against original behavior/reference output remains open.

The complete canonical list is in `docs/REMAINING.md`; avoid treating older plans or this summary as additional checklists.

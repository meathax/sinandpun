# Sin & Punishment: Star Successor Recomp

<p align="center">
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-2EA44F?logo=gnu&amp;logoColor=white"></a>
  <img alt="Windows x64" src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078D4">
  <img alt="PowerPC static recompilation" src="https://img.shields.io/badge/PowerPC-static%20recompilation-FF9F0A">
  <img alt="Status: testing" src="https://img.shields.io/badge/status-testing-8B5CF6">
</p>

A native PC port of *Sin & Punishment: Star Successor* for Wii, made with static recompilation.

The PowerPC code from the game is translated to C++ ahead of time. The Wii OS, GX graphics, audio, input, DVD and NAND
interfaces are provided by a host runtime. There is no emulator, interpreter, JIT, or PowerPC CPU in the runtime loop.

> [!IMPORTANT]
> This repository contains no Nintendo code, no game data, no extracted assets, and no compiled game binary. You must
> provide your own legally dumped copy of **Sin & Punishment: Star Successor**, NTSC-U (`R2VE01`).

<p align="center">
  <img src="docs/screenshots/title.png" alt="Recomp title screen" width="900">
</p>

## Current status

The project is in active gameplay testing:

- boots to the rendered title screen;
- all eight stage modules load and render;
- deterministic `A -> B -> A` module swaps pass;
- guest Wii Remote, Nunchuk and Classic Controller handshakes run;
- gamepad, keyboard and mouse input are implemented;
- the title and pause menus open the recomp settings overlay;
- audio produces non-silent mixed PCM;
- saves are created through the NAND HLE path;
- the release package builds and boots without bundled game data.

A full human playthrough, physical-device acceptance, audible-output testing and save/quit/reload validation remain open.
See [the current remaining work](docs/REMAINING.md) and [the full completion plan](docs/COMPLETION-PLAN.md).

<p align="center">
  <img src="docs/screenshots/gameplay.png" alt="Native gameplay rendering" width="900">
</p>

## Features

### Native runtime

- Ahead-of-time PowerPC-to-C++ translation.
- Static translation of the main DOL and all eight stage RSO modules.
- Native Aurora/GX rendering through WebGPU/D3D12/Vulkan-compatible backends.
- DVD and NAND high-level emulation against a locally extracted disc.
- Native SDL audio output, threaded AX/DSP mixing and Wii timing behavior.

### Controls

The one intentional gameplay change is modern controller support:

- left stick: Nunchuk movement;
- right stick: Wii Remote pointer aiming;
- normal gamepad triggers, bumpers and face buttons;
- absolute or relative mouse aiming;
- keyboard/mouse bindings;
- virtual Wii Remote + Nunchuk and experimental Classic Controller paths;
- remappable controls with a controller-first in-game setup UI.

### Display and graphics

- Native, 1.5x, 2x, 3x and 4x internal rendering.
- Optional FSR 1 spatial upscaling and NVIDIA DLSS where available.
- Original 60 Hz mode, match-display mode and interpolated presentation.
- VRR/G-SYNC/FreeSync support.
- Game-styled Display, Graphics, Audio, Controls, System and About pages.
- Settings are staged until **Apply and confirm** is selected, followed by a single Keep/Revert safety prompt.
- HUD/GUI/text is recomposited after 3D upscaling with nearest-neighbor sampling.

<p align="center">
  <img src="docs/screenshots/settings.png" alt="In-game graphics settings" width="900">
</p>

## Requirements

- Windows 10 or 11, 64-bit.
- A D3D12/Vulkan-capable GPU.
- .NET 8 SDK, CMake, Ninja and a 64-bit Clang/LLVM toolchain for building from source.
- Your own legally dumped **NTSC-U R2VE01** disc, extracted into a folder containing `sys/` and `files/`.

> [!NOTE]
> This project does not provide or link to game data. Do not redistribute Nintendo code or assets.

## Running

The final executable expects an extracted disc folder in `UserData/Config.toml`:

```toml
[paths]
dvd_root = 'D:/path/to/R2VE01'
```

Run `WiiCompiled.exe`. On first launch, the runtime creates its local NAND save tree under `UserData/NAND`.

## Building

Build the translator:

```powershell
dotnet build translator/Translator.sln -c Release
```

Run the data-free translator suite:

```powershell
dotnet test translator
```

Generate the translated source and build graph:

```powershell
$t = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
dotnet $t emit-native-index --project projects/r2ve01/recomp.yml --out runtime/native-index.json
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 12 --output-metadata generated/base_translation_output.json
dotnet $t generate-data-init --project projects/r2ve01/recomp.yml
dotnet $t emit-build-shards --project projects/r2ve01/recomp.yml
powershell -File tools\clean_build.ps1 -Keep
```

The eight stage modules are pre-linked and translated separately. The exact commands and expected evidence are documented
in [`docs/STATUS.md`](docs/STATUS.md).

## Controls

The default control scheme is pointer mode:

| Xbox-style input | Action |
|---|---|
| Left stick | Move |
| Right stick / mouse | Aim |
| RB | Shoot / sword |
| RT | Lock-on |
| LB | Jump |
| LT | Evade |
| Start | Pause |

Classic Controller emulation is available as an experimental option. Open the in-game settings from the game's title
or pause **OPTIONS** page; everything is remappable.

## Layout

| Path | Contents |
|---|---|
| `translator/` | PowerPC-to-C++ translator and tests |
| `projects/r2ve01/` | Main DOL and stage-module translation manifests |
| `runtime/` | Guest ABI, OS/GX/audio/input/storage HLE and settings UI |
| `aurora-main/` | Native GameCube/Wii rendering and platform backend |
| `tools/` | Translation, QA, capture, profiling and release helpers |
| `docs/` | Status, completion plan, notes and evidence summaries |
| `orig/`, `generated/`, `build/` | Ignored local disc, generated and build output |

## FAQ

**Is this an emulator?**
No. The guest PowerPC code is translated to native C++ before launch.

**Does this repository contain the game?**
No. You must legally own and dump the disc yourself.

**Is it complete?**
Not yet. All stages render and the core runtime is playable, but the final human playthrough, physical-controller
acceptance, audible-output and save/quit/reload gates remain open.

**Why is generated output not committed?**
Translation depends on the user's local disc. Ignoring generated code and local assets keeps copyrighted game data out of
the repository and keeps builds reproducible from each owner's inputs.

## AI usage

AI-assisted coding tools were used during development. Generated code and runtime behavior are validated with automated
tests, structured QA runs, screenshots and guest traces. Remaining human-only acceptance work is listed explicitly rather
than inferred from compilation.

## Credits

- The runtime and translator originated from [WiiCompiled](https://github.com/patchzyy/Wiicompiled).
- [Aurora](https://github.com/encounter/aurora) provides the GX rendering/windowing backend.
- [Dawn](https://dawn.googlesource.com/dawn) provides WebGPU.
- [Dolphin Emulator](https://github.com/dolphin-emu/dolphin) is used as a reference for Wii hardware behavior.
- The static-recompilation community and upstream open-source core contributors.

Bundled third-party components retain their original licenses.

## License

This project is licensed under the [GNU General Public License, version 3](LICENSE).

Not affiliated with, endorsed by, or associated with Nintendo or Treasure. Sin & Punishment: Star Successor is a trademark
of its respective rights holders. No Nintendo intellectual property is contained in, distributed with, or obtainable from
this repository.

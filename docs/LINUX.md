# Linux qualification

Linux support is under validation; the Windows release is not a Linux package.

## Verified on 2026-11-21

Ubuntu WSL x86_64, Clang 21.1.8: all 16 native platform tests pass, including
configuration, NAND saves, input expressions, virtual Bluetooth controller,
libco context switching and PowerPC PSQ/FMA helpers. Local evidence:
`build/linux-platform/verification.log` and `build/linux-platform/Testing/Temporary/LastTest.log`.

Run the data-free checks from Linux with CMake 3.25+, Ninja and Clang installed:

```sh
bash tools/check_linux.sh
```

The optional first argument selects a separate build directory. `CC`, `CXX` and
`CMAKE_BUILD_PARALLEL_LEVEL` override the compiler and concurrency defaults.
This does not build the translated game or exercise GPU/audio hardware.

## Full-build blocker observed

The full native configure reached CMake generation but failed because the local
`generated/build_shards/shards.cmake` and
`generated/module_shards/module_shards.cmake` contain absolute Windows `D:/...`
source paths. Those paths are not valid Linux source locations. Evidence:
`build/linux-native/configure.log`. No Linux game executable was produced.
Regenerate the translation/build inputs on Linux (including any assembly blob
paths), or make the generators emit relocatable inputs, before retrying the full
build. The dependency-free suite does not consume these manifests.

## User data

Normal installations store settings, NAND saves and logs under
`$XDG_DATA_HOME/WiiCompiled`, or `$HOME/.local/share/WiiCompiled` when XDG_DATA_HOME
is empty, absent or relative. Relative XDG paths are ignored. Explicit `--data`,
RecompLauncher data overrides and portable installations retain their existing
precedence. If an earlier build used a relative XDG path, retain that data and
use `--data /absolute/path/to/the/old/WiiCompiled` to continue using it.

## Acceptance still required

- Full native game build and data preparation from a legally dumped R2VE01 disc.
- Vulkan rendering under a real X11 or Wayland session, including resize,
  fullscreen, shader caching and frame pacing at the original 60 Hz baseline.
- Audible output and real controller/keyboard/mouse behavior, including focus loss.
- In-game save, quit and reload; all stages, bosses, continue and credits.

The WSL session used for the tests had no DISPLAY or WAYLAND_DISPLAY. Passing
the platform suite must not be reported as graphical or gameplay acceptance.

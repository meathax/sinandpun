# macOS build and validation

The runtime build has macOS targets for Apple Silicon (`arm64`) and Intel
(`x86_64`) with a default deployment target of macOS 12.0. Use one architecture
per build directory. Native build and gameplay validation on a Mac are pending.

With Apple Clang, CMake 3.25+ and Ninja installed, run the data-free tests on the
Mac from the repository root:

```sh
arch=$(uname -m)
build="build/macos-platform-$arch"
cmake -S runtime -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DMKW_BUILD_PRODUCTS=OFF -DMKW_PLATFORM_TESTS_ONLY=ON -DMKW_BUILD_PSQ_TESTS=ON
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure
```

For a full build, generate the base translation, data initialization and stage
shards on macOS from an extracted R2VE01 disc. These inputs include absolute
paths and assembly specific to the host object format. Use a separate workspace
or regenerate the inputs when switching operating systems. Configure a new build
directory with the same compiler and architecture options, leaving both
`MKW_BUILD_PRODUCTS` and `MKW_PLATFORM_TESTS_ONLY` at their defaults. The
macOS renderer uses Metal. Intel builds can use `-DMKW_X86_CPU_PROFILE=v2` for
older CPUs without the v3 instruction set.

Settings, saves and logs default to `~/Library/Application Support/WiiCompiled`.
The `--data` option can select a separate directory for playtesting. Full
acceptance still requires testing Metal rendering, audio, physical input,
save and reload, and gameplay on a Mac.

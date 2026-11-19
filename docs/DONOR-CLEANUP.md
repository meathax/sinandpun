# Donor cleanup inventory (Mario Kart Wii / Retro Rewind -> Sin & Punishment)

Written 2026-11-20. Evidence: static only (DOL section table of `orig/R2VE01/sys/main.dol`, DOL string scan,
`tools/sdk_name_map.txt`, `runtime/native-index.json`, grep). Nothing was built or run; every touched
`.cpp` was syntax-checked individually with clang++ `-fsyntax-only` using the real include/define set from
`build/native/build.ninja`.

Facts used as evidence:
* Sin DOL: text `0x80004000-0x802D5100`, `.ctors` `0x802D5100`, bss to `0x804B67E0`. Anything the runtime
  registers/reads above that (0x8052xxxx, 0x805Bxxxx, 0x8060xxxx, 0x8072xxxx, 0x809Cxxxx) is not Sin code.
* Sin links only these SDK libs: bte, homebutton, os, gx, nw4r snd/g3d/lyt/ef, dvd, wpad, sc, nand, ax, mtx, vi,
  ipc, mem, exi, si, pad, usb, dsp, arc, rso, ai, esp (9 fns), mix, axfx, kpad ... **no** SO/socket, NHTTP use, SSL,
  DWC/GHTTP, THP, RFL (Mii), Riivolution, EGG. DOL strings show `/dev/net/{kd/request,kd/time,ncd/manage,ip/top,ssl}`
  but only KD is on the boot path (`OSInit -> __OSInitNet -> NWC24iSynchronizeRtcCounter`); no `/dev/sha`, no `/dev/dolphin`.
* Boot entry is the real `__start` 0x80006310, so guest `__init_cpp` runs the real ctors.

Legend: **REMOVED** = done in this pass, **ADAPTED** = rewritten, **KEEP**, **TODO** = not done (reason).

## 1. Source files

| File | What it is | Used by Sin? (evidence) | Action |
|---|---|---|---|
| `src/hle/boot_race.cpp`, `include/boot_race.h` | `--race` boot-into-VS-race; 6 natives at 0x8052F4E8, 0x80590664, 0x805B9344, 0x8061B9BC, 0x8072624C, 0x8052F924 | native-index "ACTIVE outside text": none of these addresses are Sin code | **REMOVED** (+ `--race/--race-cpu` args in main.cpp; `--headless` kept, no longer needs a race) |
| `src/hle/strap_scene.cpp` | MKW StrapScene skip, already `SIN_DISABLED` | no | **REMOVED** |
| `src/hle/egg_decomp.cpp` | MKW EGG Yaz0 decode at 0x80218C2C, disabled | Sin has no EGG | **REMOVED** |
| `src/hle/gx/gx_egg.cpp` | EGG::DrawGX/LightTexture/AsyncDisplay, 4 natives, all disabled | no | **REMOVED** |
| `src/hle/gx/gx_fatal_stubs.cpp` | ~170 GX abort stubs, all `GX_FATAL_STUB_DISABLED` (nothing registered, nothing referenced) | no | **REMOVED** (`tools/disable_gx_stubs.py`, `tools/sin_retarget.py` still name it; they are historical) |
| `src/hle/trk.cpp` | disabled `Stub_8002001C` | no | **REMOVED** |
| `src/hle/task_thread.cpp` | MKW TaskThread/THP-retry HLE at 0x80242D7C, disabled | no THP/TaskThread in Sin | **REMOVED** |
| `src/hle/c_stdio.cpp` | disabled `ConsoleWrite` @0x80015500 (inside Sin's extab) + shared guest-printf formatter used by os_report | formatter yes | **ADAPTED** (ConsoleWrite block removed, formatter kept) |
| `src/friend_code_keyboard.cpp`, `include/friend_code_keyboard.h` | types into RR friend-code page, native @0x80601C64 | no (donor heap addr) | **REMOVED** (+ hook in `aurora_events.h`) |
| `src/discord_presence.cpp`, `include/discord_presence.h` | Discord RPC, fed by `/dev/dolphin` + main.cpp "Mario Kart Wii" | Sin never opens `/dev/dolphin` | **REMOVED** (+ config keys `[discord]`, main.cpp init/shutdown) |
| `src/hle/storage/riivolution.{cpp,h}`, `include/hle/riivolution_contract.h` | Riivolution XML overlays, RR save redirect | no | **REMOVED**; `dvd.cpp` overlay scan/by-name mapping and `nand_fs.cpp` save-redirect cut out |
| `src/hle/net/network_{core,ssl,socket,deferred,config}.cpp`, `network_internal.h`, `include/hle/network_{deferred,poll}_contract.h` | sockets, mbedTLS/Schannel SSL, DNS workers, NCD/IP-top/KD, RR Retro-WFC rules | only KD request/time are touched | **REMOVED**; replaced by `src/hle/net/network_kd.cpp` (KD request + KD time only, offline) and a 5-function `network.h`. Callers (`ios.cpp`, `nand_isfs.cpp`, `os_alarm.cpp`) lost the deferred/async-network paths. Unsupported net devices now fall through to `NAND_IOS_Open_HLE` -> `ISFS_ENOENT`, like a console with no network. |
| `include/wii_es_crypto.h` + `/dev/sha` + ES GetDeviceId/Cert/Sign in `nand_isfs.cpp` | NAS-auth crypto (Crypto++) | Sin's esp.a has none of these; no `/dev/sha` string | **REMOVED** (ES `GETTITLEID` kept) |
| `nand_isfs.cpp`: `/dev/dolphin` device, Discord ioctls | Dolphin/RR extension device | not opened by Sin | **REMOVED** |
| `nand_isfs.cpp`: `ISFS_OpenLib_Initialize`, raw `REGISTER_NATIVE_FUNCTION_AS(0x80169BCC)`, `NAND_IOS_OpenBody_HLE_801938FC` (0x801938FC) | wrote donor globals 0x80385920.. and called donor fns 0x8019AD20/80193040; both raw natives sit mid-function in unrelated Sin code | HLE-AUDIT 2: "interior, none reachable" | **REMOVED** (+ `NANDInit_HLE` in `nand_api.cpp`, disabled, wrote 0x80346D20/0x80386848) |
| `src/hle/os/os_init.cpp`: `OSInitAlarm_RecompModLateInit_801a961c`, `StaticRProlog_RecompModInit_8055531c`, `OSFatal_HLE_801a4ec4` | donor-address natives (interior of PackArgs, not code, interior of NANDInitBanner). Real OSFatal is `os_quit.cpp` 0x801AAF80 | no | **REMOVED** |
| `src/hle/os/os_alarm.cpp`: `RFLiIsWorking` (0x800BD860, reads donor 0x80386298), network completion pump | Mii lib | no RFL in Sin | **REMOVED** |
| `src/hle/sc.cpp`: `OS__IsTitleInstalled`, `OS__CheckInstall` | RMCP title-install bypass, disabled | no | **REMOVED** |
| `src/system_bridge.cpp` | ran donor static ctor ranges 0x80244DE0-0x80244EA0 (that is Sin `.text`!) and 0x8088F400.. (not code); RR marker 0x800017D8 | guest `__start` runs ctors | **REMOVED** the two loops + marker; comments no longer say MKW |
| `src/dynamic_aspect.cpp` | EGG screen-record rewriting (already `kEggScreenRecordsEnabled=false`) + Aurora viewport policy | policy only | **ADAPTED**: policy only. Renamed `ConfigureDynamicAspect/UpdateDynamicAspectSurface/SetForceAspect`; `SetMkwForceAspect` kept as inline forwarder in `aurora_events.h` for `settings_overlay.cpp` |
| `src/hle/gx/gx_dynamic_aspect.h` | EGG record math | only dynamic_aspect.cpp | **REMOVED** |
| `src/hle/gx/gx_transform.cpp` | called `AssertMkwOffscreenScreenBypass` from `GXGetViewportv` | EGG | **REMOVED** the call |
| `src/product/retro_rewind_product.cpp`, `resources/RetroRewind.{rc,ico,icns}` | RR product descriptor + icon | no | **REMOVED**; `runtime_product.h` is now just a display name ("Sin & Punishment: Star Successor", used as window/app name); `IsRetroRewind()` gone |
| `include/runtime_config.h` | MKW/RR keys | | **ADAPTED**: removed `[network]`, `[discord]`, `paths.retro_rewind_root`, `paths.overlay_roots`, `NetworkEnabled/Discord*/RetroRewindRoot/OverlayRoots`; default Config.toml text is Sin |
| `include/recomp_mod_loader.h`, `src/recomp_mod_loader.cpp` | Kamek/RR mod loader | only the **exec-write-guard + CurrentTranslatedExecutionAddress + memory reservations** are used by `memory.cpp`, `guest_flat_memory.cpp`, `dispatch.h`, `memory_access.h` | **ADAPTED**: Riivolution + DVD-overlay-root registry removed; core kept. TODO later: split the guard out and delete the rest |
| `runtime/third_party/{pugixml,cryptopp}`, `assets/certs/cacert.pem` | XML parser (Riivolution), Crypto++, CA bundle | no consumer left | **REMOVED** |
| `CMakeLists.txt`, `cmake/PublicProducts.cmake`, `cmake/RuntimeLibrary.cmake` | pugixml/cryptopp/mbedTLS targets (mbedTLS FetchContent), RetroRewind exe + `mkw_retro_*` libs, `MKW_HAVE_RETRO_REWIND`, cacert copy, `ws2_32 iphlpapi secur32 crypt32` | no | **ADAPTED** (all removed). Exe target is still named `WiiCompiled`, `mkw_release` target kept |

## 2. Items deliberately NOT changed (with reason)

| Item | Why | Next step |
|---|---|---|
| `src/music_attenuation.cpp` (+ `.h`) | `settings_overlay.cpp` (other agent) calls `MusicAttenuation::*`. Its guest part walks **MKW's SoundManager at 0x809C2898** (`kSoundPlayersOffset/Stride`); `audio.cpp:562` already disabled `TickGuest` and the `SoundPlayer::SetVolume` native is `SIN_DISABLED` | after the options UI lands: keep only the external-media monitor and drive an AX master/bus gain; delete the nw4r SoundPlayer code |
| `include/motion_trick*.h`, `src/motion_trick*.cpp`, `tests/motion_trick_tests.cpp` | MKW wheelie-by-shake; `settings_overlay.cpp` uses `MotionTrick::*` | delete with the options UI cleanup |
| `include/game_graphics_options.h`, `RuntimeConfigFile::DisabledPostProcessingPaths` | MKW ScnRenderer path mask (`& ~0x20`); `settings_overlay.cpp` uses it | delete with the options UI cleanup |
| `src/hle/game_menu_hook.cpp` | not mine; currently fails standalone syntax check (`settings_overlay::IsOpenFromGame/OpenFromGame` missing) - belongs to the options-UI agent | lead: confirm settings_overlay.h gets those |
| `MKW_GADDR*`, `MKW_GUEST_FUNC`, `kMkwGuestRegion`, `MKW_TRANSLATED_GUEST_REGION`, `mkw_*` CMake targets | translator-emitted contract (`emit-build-shards`, generated code); renaming needs translator edits | optional rename together with translator |
| `guest_abi/guest_region.h` row table | already Sin-only (R2VE01 USA/EUR/JPN rows, identity deltas) | none |
| `translator/` (`translate-mod`, `RetroRewindRegionPatches`, `Mods/Mkwii`, retro shards in `TranslatedBuildShardEmitter.cs`, `GX_FATAL_STUB` regex) | out of scope (do-not-edit) | lead decision; `shards.cmake` still emits empty `MKW_RETRO_*` sets, harmless now |
| `src/hle/storage/nand_fs.cpp` FaceLib seeding (`RFLRes01.arc`, `/shared2/menu/FaceLib/RFL_Res.dat`) | Mii resource seeding; Sin links no RFL | remove after confirming no boot-time open of that path |
| `src/hle/os/os_thread.cpp:76` `MKW_GADDR(805529A8/80552A74)` | donor task-thread entry compare (donor heap addresses) | dead compare; remove next pass |
| `assets/wii/shared2/wc24/*` + `wii_bootstrap` copy (`nand_path.h` seeds it) | harmless 110 KB first-run NAND payload; `nand_path.h` still expects it | drop when `nand_path.h` stops seeding WC24 |
| `cmake/NativePrebuilt*.cmake`, `RuntimeFingerprint.cmake` | donor's precompiled-package mode (`MKW_NATIVE_PREBUILT_DIR`, default empty) still references `mkw_cryptopp`/`mkw_mbedtls` | **that mode is now broken**; delete the three files and the `mkw_select_runtime_provider` plumbing |
| ~100 remaining `SIN_DISABLED_NATIVE_OVERRIDE*` registrations in `gx_*.cpp`, `vi.cpp`, `nand_*.cpp`, `os_*.cpp`, `audio.cpp`, `dvd.cpp` | expand to nothing; some are retargeted-pending (VI, NAND, scheduler) and the function bodies are shared with live code | per-entry pass once the owning subsystem is verified |
| `GX__DrawDone_8016eab0` registered at **0x804B2568** (`gx_stubs.cpp`) | an *active* native at a data address (donor retarget miss) | GX owner: retarget to Sin `GXDrawDone` or disable |
| Log directory prefix `base_*`, exe name `WiiCompiled.exe`, `resources/WiiCompiled.{rc,ico,icns}` | tools (`run_game.ps1`, `thread_dump.py`, `read_artifact.py`) glob `Logs\base_*` and `WiiCompiled` | rename together with tools; icon is donor art |

## 3. Verification for the lead (nothing below was run)

1. `native-index` + translation chain (natives were removed: boot_race x6, friend-code, 2x RecompMod, OSFatal 0x801A4EC4,
   ISFS_OpenLib, OpenBody, NHTTPStartup, gx_egg/egg_decomp/strap/trk/task_thread were already disabled):
   `emit-native-index`, `translate-recursive 0x80006310 ...`, `generate-data-init`, `emit-build-shards` (full chain from
   `docs/PLAN-NEXT.md` section 2), then `cmake --build build/native` **after a fresh configure** (CMake edits: removed
   pugixml/cryptopp/mbedTLS/RetroRewind targets and `third_party/{pugixml,cryptopp}`; configure must no longer try to
   FetchContent mbedTLS; `libmkw_cryptopp.a`/`libmkw_pugixml.a` in `build/native` are stale). Unity groups changed
   (fewer files), so a duplicate-static-name error is impossible to rule out blind; compile errors will name the file.
2. Expect `error:` only from `game_menu_hook.cpp` if `settings_overlay.h` is not yet updated (not my change).
3. Runtime checks: (a) `Logs\base_*\console.log` boots to `main()` exactly as before (no `[runtime] Executed N static constructors`
   lines any more - ctors now only from guest `__init_cpp`; if the title screen regresses, this is the first suspect);
   (b) KD: `IOS_Open /dev/net/kd/time` + `/dev/net/kd/request` still succeed (no `unknown device` warning for them; one
   `unknown device` warning for `/dev/net/ncd/manage` or `/dev/net/ip/top` would mean the game does use them - report it);
   (c) title -> stage load, DVD reads still published (FST entries logged), audio log (`AIRegisterDMACallback`, AX frames),
   input (`PADRead`/WPAD polling); (d) window title/app name now "Sin & Punishment: Star Successor";
   (e) `--headless` alone is now accepted; `--race` is rejected as an unknown option.
4. Behaviour changes to be aware of: `NWC24` KD request ioctls now always return success (the MKW-specific "try-suspend returns -42
   twice" state machine and generated-user-id reply were dropped); ES GetDeviceId/GetDeviceCert/Sign return `ISFS_EINVAL`.
5. Lessons DB entry added (see below).

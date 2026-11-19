# Sin & Punishment: Star Successor recomp - complete plan from here to the finish

Written 2026-11-19 for a less capable agent. Follow it literally, in order. Every step says what to do,
how to do it, what evidence proves it worked, and what to do if it fails. If a step's "Done when" is not
satisfied you are NOT done with that step, no matter how plausible the code looks.

## 0. The goal, the rules, how to behave

Goal: a complete 1:1 static recompilation (PC port) of Sin & Punishment: Star Successor (Wii, game id R2VE01,
USA) in `D:\Arcade\AI\aCORES\Sin`. The ONE deliberate change from the original: full dual-analog Xbox-style
controller support (right stick aims, left stick moves) with a controller setup UI and good aim feel.

Rules from the user (non-negotiable):
1. NEVER run `git push`. Local `git commit` is fine.
2. Before starting a task run `python C:/Users/meath/.claude/tools/lessons.py search "<keywords>" --top 5` and read hits with
   `lessons.py show <id>`. After EVERY verified fix/finding, before ending your turn, run
   `python C:/Users/meath/.claude/tools/lessons.py add --title "<symptom> -> <cause>" --class measured --project "Sin & Punishment SS recomp" --body "Symptom/Cause/Fix/Evidence"`.
3. Be blunt and honest. Never claim something works without evidence you produced this turn (command output,
   log lines, pixel stats). A successful compile is not a pass. Say clearly what does not work.
4. Show the user a screenshot each turn (`tools/run_game.ps1` makes one). If you cannot read images, use
   `python tools/shot_stats.py <png> --map` and describe the numbers; ask the user to describe the real window.
5. Clean up scratch you create (section 13). Never commit `orig/` (game data), `generated/`, `build/`.
6. Do not weaken translator strictness, tests or assertions to get past a problem. Fix the earliest cause.
7. Do not run broad `git checkout -- runtime/...` (it reverted `hle_stubs.h` three times before). Quartus rules do not apply here.
8. Do not poll or sleep-loop on background jobs. Use fixed sleeps inside one script, then read the logs.
9. Report each turn as: baseline, cause, change, evidence, risk.

### The investigation loop (use this for every problem)
1. Reproduce with `tools/run_game.ps1` (section 2). Read the key log lines.
2. Name the blocker with evidence: crash artifact backchain (`crash_*.txt`), `tools/thread_dump.py`,
   `tools/calltrace_sym.py --tail 40`, or the watchdog sampler. Never guess.
3. Form ONE hypothesis. Find the responsible code. Check whether it contains a donor (Mario Kart Wii) address (section 3).
4. Make the smallest change. Rebuild (section 2). Rerun. Compare the evidence before/after.
5. Commit, add a lesson. Only then move on.

## 1. State of the project (verified facts, as of this writing)

### Translator (`translator/`, .NET 8; fork of Wiicompiled, GPL-3)
- `main.dol` translates: 11,169 functions, 0 unsupported, strict mode. Test suite: 767 pass / 0 fail / 8 skip.
- All 8 stage modules translate (`projects/r2ve01/stg00.yml`..`stg07.yml`, output `build/generated-stg0N/`). They are NOT yet wired into the runtime (M3).
- Project manifest: `projects/r2ve01/recomp.yml`. Output root: `generated/`. Module images: `build/modules/stg0N.bin`.

### Runtime (`runtime/`, `aurora-main/`)
- Builds/links to `build\native\WiiCompiled.exe` (clang 22 LLVM-MinGW, `-march=x86-64-v3`; translated code `-fno-fast-math -ffp-contract=off`). Full rebuild about 26 seconds.
- Boot reaches: OSInit -> `main()` -> engine init -> a real 60 Hz frame loop. The game builds an nw4r g3d `ScnRoot` and runs its scene draw path (verified in the call trace: `DrawOpa`, `GXSetFog`, `MTXLookAt`).
- BUT: the window is black (PrintWindow capture: 338 colours, ~90% pure black plus a small text line at screen centre of unknown content), ZERO DVD file reads ever happen (no `DVDReadPrio`/`ReadAsync`), and the game polls `PADRead`, `WPADProbe`, `DVDGetDriveStatus`.
- Guest threads at the last dump: main (pri 16, waiting on the VI retrace queue each frame), a pri-3 worker (entry 0x80280CC0) and a pri-4 worker (entry 0x8027C040), both parked on OS message queues, plus the SDK idle thread (deliberately never scheduled; the host idle loop replaces it).
- `WPADInit` (0x801C0C10) used to be HLE-skipped (`runtime/src/hle/input/wpad.cpp`), so the guest never connected a Wii Remote. The last action was to DISABLE that skip (`SIN_DISABLED_NATIVE_OVERRIDE_VOID`) and run the real guest WPAD/WUD against the Bluetooth HLE with `SIN_VIRTUAL_REMOTE=1 SIN_CALLTRACE=1`. That run was launched in the background and its result is UNKNOWN to this document. Redo it yourself as M1 step 1.

### Fixes already made (all documented in the lessons DB; search before repeating work)
- Decrementer simulation (`PPCMtdec`, `runtime/src/hle/os/os_init.cpp`): callback ABI is r3 = exception number 8, r4 = context; `OS__LoadContext` on the interrupted context throws `DecrementerExceptionReturn` (caught in `PPCMtdec`, which restores a snapshot of the CpuContext). `OS__LoadContext` is no longer `[[noreturn]]`; SRR0 = 0 means "stay on current execution".
- `OSInitFont` (0x801AC710) HLE writes a blank ANSI font header and returns TRUE (real one spins forever in `__OSReadROM`, no EXI model).
- Scheduler addresses retargeted to Sin: RunQueue 0x8042A5F0, DefaultThread 0x8042A2D8, IdleContext 0x8042A6F0, IdleThread 0x8042A9B8, Reschedule 0x804B22A0, RunQueueHint 0x804B22A4, RunQueueBits 0x804B22A8, SwitchThreadCallback pointer 0x804B0BF0, AlarmQueue 0x804B2220 (in `os_internal.h`, `fiber_manager.cpp`, `os_alarm.cpp`).
- `SelectThread` (`os_scheduler.cpp`) adopts fiberless guest-created threads (the guest's own translated `OSCreateThread` runs; donor thread HLEs are disabled) and ignores bit 0 of RunQueueBits.
- Diagnostics: `SIN_WATCHDOG_SECS=N`, `SIN_CALLTRACE=1`, `tools/screenshot.ps1` (PrintWindow), plus the helper scripts in section 2.

### Uncommitted work (run `git status --short`; commit it first, one clear commit)
- NEW `runtime/include/virtual_remote.h`, `runtime/src/wiimote/virtual_remote.cpp`: gamepad-backed virtual Wii Remote + Nunchuk (details in M4). Compiles, untested end to end.
- `runtime/src/hle/bluetooth/bt_usb_device.cpp`: Bluetooth Controller now constructed with `VirtualRemote::Host()`.
- `runtime/include/dual_analog.h` (Nunchuk range +-96), `runtime/src/hle/input/wpad.cpp` (WPADInit HLE disabled), `runtime/src/main.cpp` (watchdog + RIP sampler), `runtime/src/hle/vi.cpp` (temporary `[vi] VIWaitForRetrace` log: rate-limit it or delete it), `tools/*` helper scripts, `docs/PLAN-NEXT.md`, `GOAL.md`.

## 2. Tools and commands (memorize these)

All commands from the repo root `D:\Arcade\AI\aCORES\Sin`, in PowerShell (pwsh).

### Rebuild
- Only plain C++ edits in `runtime/`: just run the cmake step.
- If you added/removed/changed the address of any `PPC_NATIVE_OVERRIDE*`, `SIN_DISABLED_NATIVE_OVERRIDE*` or `GX_FATAL_STUB`, run the whole chain:
```
$t = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
dotnet $t emit-native-index --project projects/r2ve01/recomp.yml --out runtime/native-index.json
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 12 --output-metadata generated/base_translation_output.json
dotnet $t generate-data-init --project projects/r2ve01/recomp.yml
dotnet $t emit-build-shards --project projects/r2ve01/recomp.yml
Get-Process WiiCompiled -ErrorAction SilentlyContinue | Stop-Process -Force
& "C:/Users/meath/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/cmake.exe" --build build/native --parallel 30
```
Always kill any running `WiiCompiled` before the cmake step ("Permission denied" at link time otherwise). Compile errors print as `error:` lines; filter with `| Select-String error`.
- A CHANGE to `runtime/include/guest_abi/*.h` rebuilds everything (still only ~30 s).

### Run + capture
`powershell -File tools\run_game.ps1 -Seconds 50 -Name m1 -Env SIN_VIRTUAL_REMOTE=1,SIN_CALLTRACE=1`
It starts the exe, waits, takes `build\screenshots\<Name>.png` (PrintWindow, the real game window), closes the game, then prints key log lines, screenshot pixel stats, and pointers to the newest crash artifact and call trace.
Environment switches the runtime understands: `SIN_VIRTUAL_REMOTE=1` (use the virtual remote even without a gamepad), `SIN_CALLTRACE=1` (write `build\native\calltrace.txt`), `SIN_WATCHDOG_SECS=N` (after N s, then every N s, write a crash-style artifact AND print `[watchdog] sample ...` lines naming the guest function each host thread is executing).

### Evidence tools
- `python tools\shot_stats.py build\screenshots\x.png --map`: colours, non-black %, bounding box, ASCII map. Blank window = ~338 colours / ~90% black. A rendered frame = thousands of colours and a big non-black bounding box.
- `python tools\calltrace_sym.py --tail 60` / `--grep "DVD|NAND"` / `--diff old_calltrace.txt`: execution order of new guest call targets, SDK names filled in. Save a copy of `calltrace.txt` as `build\calltrace_<name>.txt` before changing things so you can `--diff` it later.
- `python tools\thread_dump.py build\native\UserData\Logs\base_XXXX`: all guest threads (state 1 ready, 2 running, 4 waiting), priorities, wait queues, scheduler globals.
- Crash artifacts: newest `build\native\UserData\Logs\base_*\` contains `crash_*.txt` (reason, guest registers, backchain with `fn_XXXXXXXX+offset` names), `console.log`, `mem1.bin` (guest MEM1, BIG-ENDIAN, base 0x80000000; read with python `struct.unpack('>I', ...)`, never with PowerShell BitConverter), `mem1.bin.mem2`.
- Generated code for any guest function: `Select-String -Path generated\build_shards\base_common\*.cpp -Pattern 'extern "C" void func_8003EC1C\('` then read around the hit. Calls look like `InvokeDirectCpu<0xADDR>(ctx)` or `MKW_STATIC_TRANSLATED_CALL(0xADDR, func_ADDR, ctx)`. Memory reads look like `MemoryInline::FlatRead32(r3 + 16)`. `ctx->lr = 0x...` just before a call is the return address.
- SDK names for guest addresses: `Select-String tools\sdk_name_map.txt -Pattern 'DVDOpen'` (tab-separated: address, SDK name, library, match quality; `diff=0` = byte-exact match, trust these).
- SDK headers: `build\sdk\x\wii_development_package\RVL_SDK\include\revolution\<module>\*.h`. ALWAYS read the real struct/typedef before emulating or calling any SDK structure or handler.
- Finding the address of an SDK global without symbols (how the scheduler constants were found): r13 (small-data base) = 0x804B7700 and r2 = 0x804BA920. Scan the translated SDK function's instruction words in `mem1.bin` for `lwz/stw rX, imm(r13)` (opcode bits 32/36, rA = 13) and compute `0x804B7700 + signed imm`. Do the same for a function that obviously uses the global (e.g. `SelectThread` 0x801AFBB0 uses RunQueueBits).
- `tools\ppc_dis.py` is crude: its register names are wrong for many opcodes. Trust only immediates/opcodes, or decode fields yourself (op = w>>26, rD = (w>>21)&31, rA = (w>>16)&31, imm = sign-extend(w&0xFFFF)).

### Adding or changing a native (HLE) function
- Syntax at the end of the function's cpp: `PPC_NATIVE_OVERRIDE(0x80XXXXXX_without_0x, MyFunction, uint32_t, (uint32_t arg0), (arg0));` (see `runtime/src/hle/os/os_init.cpp` OSInitFont for a working example). Arguments arrive per the PowerPC ABI (r3.. = args, return in r3). `PPC_NATIVE_OVERRIDE_VOID(addr, fn, (CpuContext* ctx), (ctx))` gives you the raw CPU context.
- Use the SDK address from `tools\sdk_name_map.txt` (never a Mario Kart Wii address).
- To disable a native so the real guest code runs: change the macro name to `SIN_DISABLED_NATIVE_OVERRIDE` / `SIN_DISABLED_NATIVE_OVERRIDE_VOID`.
- Then run the full rebuild chain.
- Guest memory access from C++: `::Memory::Read32(addr)`, `Read16`, `Read8`, `Write32`, `Write16`, `Write8` (big-endian handled for you).

## 3. The central bug class: leftover Mario Kart Wii (donor) constants

The runtime is forked from a Mario Kart Wii recomp. Any MKWii address in HLE code is wrong in Sin and is a latent crash or silent hang. Symptoms so far: threads never scheduled, scheduler reading garbage, jumps to NULL. Search for them:
```
Select-String -Path runtime\src\*.cpp,runtime\src\*\*.cpp,runtime\src\*\*\*.cpp -Pattern 'MKW_GADDR|MKW_GUEST_FUNC'
```
Donor addresses look like `0x8034xxxx`, `0x8035xxxx`, `0x8038xxxx`, `0x8012xxxx`, `0x801Axxxx` that do not appear in `tools/sdk_name_map.txt`. Sin's SDK is mostly at 0x80150000-0x801F0000 and Sin's SDK data/bss at 0x8042xxxx-0x804Cxxxx. For each hit in code that is active (not disabled):
1. Find what it is (function or global) from the surrounding code and the SDK header.
2. Find the Sin address: function -> `tools\sdk_name_map.txt`; global -> SDA scan (section 2) or the SDK `.a` member `.bss` layout (`tools\os_bss_full.py` shows how it was done for OSThread.o).
3. Replace it with a comment naming the evidence, OR disable the whole HLE.
Known unaudited places: `runtime/src/hle/vi.cpp` (many VI state addresses at 0x8035xxxx/0x8038xxxx), `os_init.cpp` (0x801A961C, 0x8055531C), `ax_effects` 8012B830, `network_config` 801D9E94/801D8D30, disabled thread/mutex HLEs in `os_thread.cpp`/`os_scheduler.cpp`. Audit a subsystem the moment the game starts using it (a subsystem that is not exercised does not matter yet).

## 4. Milestones overview (do in order)

| ID | Milestone | Done when (hard evidence) |
|----|-----------|---------------------------|
| M0 | Commit current work | `git status` clean of source changes, one commit, lesson added |
| M1 | Controller gate solved: real guest WPAD connects the virtual remote | Log shows virtual remote connected; guest `WPADProbe` ok; call trace gains new functions |
| M2 | First real frame: DVD reads + visible picture | `[dvd]` reads in log; `shot_stats` shows thousands of colours; screenshot of a title/logo screen |
| M3 | Stage modules (RSO loader + code-set switching) | Starting a stage loads `stg0N.bin` and runs gameplay frames without a fatal |
| M4 | Dual-analog aiming + setup UI | Right stick moves the reticle correctly (measured), left stick moves the character, UI persists settings |
| M5 | Full playthrough validation and polish | All stages/bosses/menus/save/audio verified against the original; final acceptance checklist (section 11) |

If a milestone blocks you for more than a few full attempts, stop and write what you know (evidence, hypotheses ruled out) in `docs/STATUS.md`, add a lesson, and ask the user.

## 5. M0 - Commit the current work

1. `git status --short`. Review what is listed; `orig/`, `generated/`, `build/` must not appear (they are gitignored; if they do, stop and fix `.gitignore`).
2. Remove or rate-limit the temporary `[vi] VIWaitForRetrace call` log in `runtime/src/hle/vi.cpp` (search `s_waitLogs`; keep at most the first 3 calls).
3. Rebuild (section 2) and make sure it compiles.
4. `git add -A runtime tools docs projects GOAL.md; git commit -m "Virtual gamepad remote, call trace/watchdog diagnostics, run/inspect tools, plan"`. Do not push.
5. Add a lesson about the virtual remote design and the new tools.

## 6. M1 - Get the controller connected through the real guest WPAD

Why: the game polls the pads and never starts loading assets. With the donor-era `WPADInit` skip, the guest sees no controller at all.

### M1.1 Run the real WPADInit and read what happens
1. Make sure `wpad.cpp` has `SIN_DISABLED_NATIVE_OVERRIDE_VOID(801C0C10, ...)` and run the full rebuild chain.
2. `powershell -File tools\run_game.ps1 -Seconds 60 -Name m1a -Env SIN_VIRTUAL_REMOTE=1,SIN_CALLTRACE=1`
3. Possible outcomes and what to do:
   - A. Crash (a `crash_*.txt` artifact, or the log ends with `FATAL`/`reason:`). Read `crash_*.txt`: the `Guest refs` line (pc, lr, ctr) and `Stack code refs`/`backchain` name the failing guest function. Apply section 3: the crash is almost always a donor address or an HLE that returns the wrong thing. Fix, rebuild, rerun. Previously seen (before the alarm/scheduler fixes): a jump to 0x801AADE0 (interior of `fn_801AAB30`) from a garbage alarm callback registered by WPADInit. Alarm queue and `InsertAlarm` are now retargeted, so check whether it still occurs.
   - B. No crash, no `Virtual Wii Remote ... connected` line, the game just idles. The guest started the Bluetooth stack but something never completes. Go to M1.2.
   - C. The log has `Virtual Wii Remote + Nunchuk connected to the game (gamepad)`. Go to M1.4.

### M1.2 If the Bluetooth stack stalls: find where
The path: guest WPAD -> WUD -> `/dev/usb/oh1/57e/305` IOS device (opened via the guest's real IPC code) -> HLE in `runtime/src/hle/bluetooth/bt_usb_device.cpp` -> HCI/L2CAP/HID emulation in `runtime/src/hle/bluetooth/bt_controller.cpp` -> `RemoteHost` interface (`runtime/include/bluetooth/bt_controller.h`) -> `VirtualRemote::Host()` (`runtime/src/wiimote/virtual_remote.cpp`).
1. Add rate-limited `RT_LOG(RT_TAG_HLE)` lines (first ~40 events) in: the IOS device open/close, every ioctl/ioctlv (command number + sizes), every HCI command opcode in `Controller::ExecuteCommand`, and each `RemoteHost` call in `CompositeHost` (IsAttached, ConsumeWake, SendOutputReport report id, PopInputReport report id). Log format `[bt] ...`.
2. Run, then read the `[bt]` lines in order. Typical expected sequence for a connect: device opened -> HCI reset / read local info / write scan enable -> inquiry (the remote shows up) or the remote "wakes" (`ConsumeWake` returns true once) -> connection request -> L2CAP connect on PSM 0x11 (HID control) and 0x13 (HID interrupt) -> `OnLinkStateChanged(slot0, Ready)` -> the guest sends output reports (status request 0x15, LEDs 0x11, report mode 0x12, memory reads/writes 0x16/0x17).
3. Find the first step that does not happen and compare with the older MKWii flow it was written for. Differences to check: IOS ioctl numbers/versions Sin's WUD uses (Sin's SDK is 2009; the donor targeted an earlier one), response buffer sizes/contents (the previous agent noted "the versioned exchange leaves response buffers uninitialized"), `ConsumeWake`/`IsAttached` timing.
4. Fix in the HLE. Rerun until the connect line appears.

If the real WPAD path proves impossible after serious effort (you have evidence of what the guest requires that the HLE cannot provide), STOP and ask the user before using a fallback that writes KPAD/WPAD guest structures directly. That fallback is not 1:1 and must be the user's decision.

### M1.4 After the connect: handshake correctness
Expected guest traffic after the link is Ready (log each output report id once, rate-limited):
- 0x15 status request -> we answer 0x20 with the extension bit set.
- 0x16 writes: `0xA400F0 = 0x55`, `0xA400FB = 0x00` (disable extension encryption), IR block writes at `0xB00030`, `0xB00000` (9 bytes), `0xB0001A` (2 bytes), `0xB00033` (IR mode).
- 0x17 reads: extension ID at `0xA400FA` (6 bytes; we return `00 00 A4 20 00 00` = Nunchuk), calibration at `0xA40020` (16 bytes), EEPROM calibration at `0x0016`.
- 0x12 report mode (expect 0x37: accel + 10 IR bytes + 6 extension bytes), then continuous reports.
Every read the guest issues that gets `error=8` (unmapped) in `virtual_remote.cpp` `ReadMemory` is a bug to fix (add the region). Every write answered with an error code is also a bug.
If the guest keeps re-requesting or disconnects: a checksum or ID mismatch. The EEPROM accel calibration checksum and the Nunchuk calibration checksum are `sum + 0x55` (and `sum + 0xAA` for the second Nunchuk byte); compare with Dolphin's emulated Wiimote if the guest rejects them.
Done when: log shows virtual remote connected, report mode set, reports flowing; `WPADProbe`/KPAD report a Nunchuk-equipped remote (confirm via call trace: new KPAD functions appear) and the call trace after the controller check gains new targets (`python tools\calltrace_sym.py --diff build\calltrace_before.txt`).

## 7. M2 - First real frame: DVD reads and a visible picture

### M2.1 Find out what the game waits for now
1. After M1, run with call trace and the watchdog: `-Env SIN_VIRTUAL_REMOTE=1,SIN_CALLTRACE=1,SIN_WATCHDOG_SECS=30`.
2. `python tools\thread_dump.py <newest artifact>` and `python tools\calltrace_sym.py --grep "DVD|NAND|ISFS|ARC|File|Read"`. Is any `DVDOpen`/`DVDReadPrio`/`DVDReadAsyncPrio`/`DVDFastOpen` ever called? If no: the game has not started loading. If yes but it stalls: see M2.3.
3. If the game still idles with a connected remote, find what the main loop waits for. The main frame loop is `fn_8003EC1C` (generated code in `generated\build_shards\base_common\shard_618db9a2a11ef997e37894ed.cpp`); the object it works on is in `r30`. Read the loop body for the condition guarding the next state (flags read from `r30+N`, calls to subsystem functions). Use `calltrace_sym.py --from <n>` to see which functions ran right before it stopped making progress. Likely candidates: `DVDGetDriveStatus` (0x801916B0, real guest code over HLE `DVDLow*`) returning a not-ready/error status, a NAND/ISFS operation never completing (`NANDPrivateOpenAsync`, `ISFS_OpenAsync` complete through the IOS IPC HLE: check completion callbacks fire), a message queue the worker threads wait on that nobody posts to (OS message-queue HLE is in `runtime/src/hle/os/os_msg*.cpp`; log `[msg]` lines show PARKING), or a timer/alarm that never fires.
4. For DVD: the HLE is `runtime/src/hle/dvd.cpp` (registered natives `DVDReadPrio` 0x8018D270, `DVDReadAsyncPrio` 0x8018D180, `DVDReadAbsAsyncPrio` 0x80191410, `DVDLow*` 0x801932E0-0x80195640). `DVDInit` 0x8018D480 and the driver state machine (`cbForState*`, 0x8018F2B0..0x80190A30) are real guest code. Disc files come from `UserData\Config.toml` `dvd_root` (261 files indexed, FST published at 0x97DC0000 - verify the guest's `DVDConvertPathToEntrynum` (0x8018CDD0) finds files). Check the guest-visible globals the real code reads (e.g. the boot info at 0x80000000-0x80000100 such as disc id, FST pointers at 0x80000038/0x80000034) are populated.

### M2.2 Make the picture visible
Once assets load the game draws for real. If the window stays black while the call trace shows draw calls:
1. Present path: `GXCopyDisp` -> `VI_HLE_SetXfbReady` -> `VISetNextFrameBuffer` -> `VIFlush` -> `VI_HLE_PresentFrame` in `runtime/src/hle/vi.cpp`; GX HLE in `runtime/src/hle/gx/`; renderer in `aurora-main/`. Add rate-limited logs at each stage and see which is never reached. Fix donor addresses in `vi.cpp` (section 3) first: wrong VI state addresses can make the presenter think nothing is ready.
2. The game starts with `VISetBlack(TRUE)` and un-blacks after its first completed frame; confirm `VISetBlack(FALSE)` runs.
3. If GX commands are issued but nothing renders, compare against how aurora expects them (the donor game was also a GX game; most GX HLE is generic). Look for `GX_FATAL_STUB`-style disabled functions or logged `unsupported`/`not implemented` messages: `Select-String build\boot-err.log -Pattern 'unsupported|not implemented|TODO'`.
Done when: `python tools\shot_stats.py` on a PrintWindow screenshot shows thousands of colours and a large non-black bounding box. Save the screenshot as `build\screenshots\m2-first-frame.png`, show it to the user, and describe what it shows.

### M2.3 Loads that stall or crash
Every crash from here on is handled by the investigation loop (section 0). Common ones: a missing native for an SDK function the real guest code needs (an unhandled MMIO/hardware access: Memory layer reports it), an unaligned/`lmw`-style instruction path, a missing indirect-jump target (the log names the address: "switch jump table the translator missed" - report to the translator side: re-run translation with that address added to the project manifest `projects/r2ve01/recomp.yml` entry/seed list and rebuild), audio (AX) or NAND save-file operations needing an HLE answer.

## 8. M3 - Stage modules: RSO loader HLE and per-module code sets

Background: the game's 8 stage modules (`stg00`..`stg07`) are RSO/REL-style relocatable objects that the game links at one shared address window starting at 0x80800000. Only one is resident at a time. The translator already produced code for each module (functions per module: stg00 4,987; stg01 6,225; stg02 6,116; stg03 5,190; stg04 5,134; stg05 6,605; stg06 8,215; stg07 6,108). Pre-linked images are `build\modules\stg0N.bin`.
Guest loader call chain (found earlier): `fn_8001F25C` -> `0x8020E8BC` -> `RSOStaticLocateObject` (0x8020D990).

### M3.1 Observe what the game actually does at stage load
1. Reach the point where the game starts a stage (needs M2). Use the call trace around `fn_8001F25C`/`0x8020E8BC`/`0x8020D990`; read their generated code to learn the arguments (module file name/ARC entry, destination address, relocation handling). Also find what the game does with the module afterwards: it calls into module functions through function pointers or `bctr` into 0x808xxxxx.
2. Decide the integration contract: "when the game asks to load module N into 0x80800000, the runtime copies the pre-linked image `stg0N.bin` there and activates module N's code set".

### M3.2 Translator side: per-module registration
Currently `generate-data-init`, `emit-build-shards`, `emit-native-index` are guarded against `inputs.module` (they only handle `main.dol`). Extend the translator CLI (`translator/src/Translator.Cli/`, find these commands with `Select-String -Path translator\src -Pattern 'emit-build-shards' -Recurse`) so each module manifest emits its own shard set and a registration function the runtime can call to (de)activate that module's functions (registry swap). Keep the translator test suite green (`dotnet test translator` -> expect 767+ passed, 0 failed). Use per-module output trees `build\generated-stg0N\` and add them to the runtime CMake so they compile into separate static libs or object sets.

### M3.3 Runtime side: activation
1. Intercept the stage-load entry with a native override at the right address (decide the interception point from M3.1: the highest level function where the module bytes are known, so you do not have to emulate file reading and relocation). Copy `stg0N.bin` to 0x80800000 (`Memory` write), then call the module's registration function after unregistering the previously active module (look at `TranslatedFunctionRegistry` in `runtime/include/guest_abi/dispatch.h` and `runtime/src/abi_bridge.cpp` for register/unregister and cache invalidation; every cached lookup (`static std::atomic` caches in `dispatch.h` such as `IsBaseTranslatedCpuTargetActive`) must tolerate the swap - verify, do not assume).
2. Answer `RSOStaticLocateObject` symbol queries (0x8020D990) from the module's symbol table (see the translator's module metadata) or run the real guest function once the module image is in place.
3. Run the stage-0 load, then the other seven, one at a time.
Done when: choosing each stage loads without a missing-target fatal and gameplay frames render; the call trace shows module functions (0x808xxxxx) executing. Stage-switch twice in a row (stage A -> B -> A) to prove the swap is clean.

## 9. M4 - Dual-analog aiming and controller setup UI (the mandated feature)

### Design (already implemented, untested end-to-end)
The runtime presents a virtual Wii Remote + Nunchuk to the emulated Bluetooth controller, driven by an SDL gamepad. The game's own KPAD/WPAD computes the pointer from synthetic IR dots, exactly as on hardware, so aiming behaves 1:1; only the input source changes.
- `runtime/src/wiimote/virtual_remote.cpp`: `VirtualWiimote` (device protocol) and `CompositeHost` (slot 0 = virtual remote when no physical remote is attached and a gamepad exists or `SIN_VIRTUAL_REMOTE=1`; other slots and physical remotes pass through to `Wiimote::Host()`).
- `runtime/include/dual_analog.h`: deadzone, gamma response curve, stick-as-velocity pointer integration (`Advance`), IR dot pair (`EmitDots`), Nunchuk mapping.
- Default mapping (revise after reading the game's real control scheme): right stick = pointer; left stick = Nunchuk stick; RT or RB = B; face A/B = A; LB = C; LT = Z; X = 1; Y = 2; Start = +; Back = -; Guide = Home; d-pad = d-pad.
- Gamepad source: `PADGetSDLGamepadForIndex(0)` (aurora). The game overlay blocks input via `InputBindings::InputBlocked()` (already respected).

### M4.1 Make the pointer correct (needs M1 and a rendered reticle from M2)
1. Add a deterministic test input: environment variable `SIN_VREMOTE_SCRIPT` (parsed in `VirtualWiimote::ReadInput`) that, when set, ignores the gamepad and sweeps the right stick through a fixed sequence (e.g. 3 s right, 3 s left, 3 s up, 3 s down, 3 s idle) so a run needs no human. Also log the synthesized IR dot centre and (rate-limited) the pointer coordinates the game computes (read KPAD output from guest memory, or add a log inside the guest `KPADRead` result via a debug-only native).
2. Capture screenshots at the end of each script segment with `tools\shot_stats.py --map` (or ask the user to look). Verify: stick right -> reticle right, stick up -> reticle up. If flipped, negate the corresponding axis in `BuildDataReport` (`DualAnalog::Advance(m_pointer, m_config, -in.rx, -in.ry, dt)`), nothing else.
3. Verify range: full stick sweep must reach all screen edges without losing the pointer (if the pointer jumps/disappears at an edge, widen `cameraMinX/MaxX/MinY/MaxY` or reduce `dotSeparation` in `DualAnalog::Config`). KPAD needs both dots inside the 1024x768 camera. Sensor bar position (above/below TV) is a SYSCONF setting (`BT.BAR`, generated by `tools\gen_sysconf.py`); keep it consistent with the dot geometry.
4. Verify buttons: each mapped button reaches the game (use the game's pause/menu navigation as a test) and the Nunchuk stick moves the character (left stick).
5. GameCube-controller path: `PADRead` (0x801B4210) is already HLE'd in `runtime/src/hle/input/pad.cpp`. Check in the game's options whether it offers a GC control scheme; if so make the right stick feed its aiming too.

### M4.2 Aim feel (do this after M4.1 is correct)
Tunable parameters live in `DualAnalog::Config`: `radialDeadzone` (0.12), `responseGamma` (0.65), `maxSpeedX/Y` (900/700 camera px per second), `dotSeparation`. Improve in this order and verify each by feel with the user:
1. Frame-rate independence (already uses dt; clamp dt to 50 ms).
2. A precision mode (hold a button, e.g. LB, to scale speed to ~35%).
3. Optional small acceleration ramp for the first ~120 ms of deflection.
4. Re-centre option (button recentres the reticle).
5. Absolute mode alternative (stick position maps to pointer position) selectable in the UI.
Never modify game logic; only the synthetic IR input.

### M4.3 Controller setup UI
Existing overlay code (inherited from the MKWii port): `runtime/src/settings_overlay.cpp` (ImGui settings), `runtime/src/controller_mapping_wizard.cpp` (button mapping wizard; its step text still says "accelerate/brake" - rewrite for Sin), `runtime/src/input_bindings.cpp`. Work:
1. Read the game's real controls from the game's own control/option screen (or the Sin & Punishment: Star Successor manual) and name actions correctly in the UI: do NOT invent names.
2. Add a "Controller" tab: sensitivity X/Y sliders, right-stick deadzone, response curve (gamma), max speed, pointer mode (velocity/absolute), invert Y, precision-mode button and scale, left-stick deadzone; button remapping with a "press the button for <action>" wizard; a live preview widget showing both sticks and the pointer (read the same `VirtualWiimote` state through an accessor).
3. Persist to the existing config file (see how other settings are saved in `settings_overlay.cpp`; `UserData/Config.toml`). Settings must apply live (no restart) via `DualAnalog::Config` held in a shared struct guarded by a mutex or atomics.
4. The overlay must block game input while open (`InputBindings::InputBlocked()`), restore on close, and never steal focus from the game window.
Done when: every setting changes behaviour live, survives a restart, resetting to defaults works, and the user confirms the feel.

## 10. M5 - Full validation against the original, polish

1. Play through, in order: boot/logos, title, options, each of the stages (stg00..stg07) including bosses and cutscenes, game over/continue, pause, quit, save/load, credits. For each, record: runs without fatal, picture plausible, audio plays, controls work, no stuck states. Keep a table in `docs/STATUS.md` (stage, result, issues, evidence screenshot).
2. Known fidelity risks to check: floating-point behaviour (translated code is `-fno-fast-math -ffp-contract=off`; if physics/AI diverge investigate paired-single/quantized loads in `runtime/src/ppc_quantized.cpp` first), pacing (60 Hz retrace-driven; no speedups or slowdowns), audio (AX/nw4r SND HLE in `runtime/src/hle/audio/`), save data (NAND/ISFS HLE; save, quit, reload), THP/cutscene video, text rendering (OSInitFont is a blank stub: if any in-game text uses the IPL font it will be missing; verify the game uses its own fonts and nothing shows blank), widescreen/aspect handling, resolution/scale options.
3. Compare to the original: ask the user for reference videos/behaviour, or a known-good emulator run; never assume.
4. Performance: the frame loop should hold 60 fps on the user's machine (i9-13900KF, RTX 4090). Profile with the watchdog sampler (`SIN_WATCHDOG_SECS`) if not.
5. Cleanups: remove debug-only logs; keep `SIN_CALLTRACE`/`SIN_WATCHDOG_SECS` as documented debug switches. Update `README`/`docs/STATUS.md` with: how to build, how to point `dvd_root` at an extracted disc, control mapping, known issues, license (GPL-3, forked from Wiicompiled; no game data included).

## 11. Final acceptance checklist (all must be true, each with evidence in `docs/STATUS.md`)
- [ ] Clean build from scratch succeeds (translator + runtime) with the documented commands.
- [ ] Translator tests pass (767+ / 0 failed).
- [ ] Boot to title with a visible picture and audio (screenshot + log).
- [ ] All 8 stages load and play (module swap proven, table of results).
- [ ] Controller: right stick aims correctly (measured with the scripted sweep), left stick moves, all buttons mapped, setup UI persists settings.
- [ ] Save/load works.
- [ ] No fatal errors in a full playthrough; no unbounded log growth.
- [ ] `git status` clean, everything committed locally, nothing pushed.

## 12. When you are stuck
1. Re-read the section 0 loop. Did you gather evidence first, or guess?
2. Search the lessons DB (`lessons.py search`), `docs/STATUS.md`, and git log (`git log --oneline -30`) for the same symptom.
3. Check section 3: is a donor address involved?
4. Reduce: find the first divergence between what the guest does and what the HLE answers (log both sides).
5. If still stuck after several honest attempts: write the evidence and ruled-out hypotheses in `docs/STATUS.md`, add a lesson, and ask the user a specific question.

## 13. Cleanup (mandatory every time)
Delete scratch you created: `build\native\calltrace.txt` (copy it to `build\calltrace_<name>.txt` if you still need it, delete later), old watchdog/crash artifact dirs in `build\native\UserData\Logs\base_*` (keep only the newest one that is current evidence), most of `build\screenshots\*` (keep milestone proofs), any temp dumps. Never delete another running job's files, anything under `orig/`, or files modified in the last 3 days that you did not create. State in your final message what you removed.

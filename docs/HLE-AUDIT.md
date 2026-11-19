# HLE audit: OS / DVD / NAND / VI / PAD / SC / EXI / fonts / quit flow (R2VE01)

Written 2026-11-20 by the "everything else" audit agent. Scope: every non-GX, non-audio, non-WPAD
SDK surface the game uses in gameplay. Nothing here was run: no cmake, no game launch. All evidence
is static (main.dol disassembly via `tools/dis_dol.py`, static call edges via `tools/hle_callers.py`,
the `runtime/native-index.json` registry and the HLE sources). Items marked **UNVERIFIED** need a run.

Tools added (all read-only analysis, safe to keep):

* `tools/hle_retarget_report.py` - per SDK function: ENABLED native / DISABLED donor HLE / guest, plus
  `--hw` = functions that load `lis 0xCC00/0xCD00` (hardware registers) with no native.
* `tools/hle_callers.py` - static caller census (game callers vs SDK callers, `--reach` transitive).
* `tools/dis_dol.py ADDR [n|fn]` - small PPC disassembler with SDK names.

## 0. Findings that matter most (read this first)

1. **Donor (MKWii) constants corrupted game `.bss` on every frame / read.** R2VE01 `.bss` is
   0x80334680-0x804B68C0 and `MKW_GADDR()` is identity for Sin, so a leftover donor address is a write
   into an unrelated game variable. Active offenders found and removed (this audit):
   * `vi.cpp` `WriteGuestStateLocked` (every retrace): u8 @0x80386B44, u16 @0x80350864/66/72/7C.
   * `dvd.cpp` `CompleteDvdCancelState` (after every HLE DVD read/callback): 8x32-bit words @0x80343230
     (queue self-links), @0x8038667C, @0x803866A8, plus unverified guesses @0x804B2034/0x804B20A4.
     `DVDInit_8015EA1C` (still run from `DVDLowInit`, boot) wrote @0x80386724/25, @0x804B2250 and a
     128-byte context table @0x803434E0.
   * `os_interrupt.cpp` `IPCCltInit` (boot): +0x1000 into word @0x803867EC.
   * `os_init.cpp` `__OSInitSTM` (boot): 3 words @0x80386934/38/3C.
   * `sc.cpp` `SCGetProductCode`: up to 7 bytes @0x803869E0 (+ wrong lookup tables, see 3.5).
2. **Quit / reset / HOME-menu flow had no HLE.** `OSShutdownSystem`, `OSRestart`, `OSReturnToMenu`,
   `OSReturnToDataManager` end in the synchronous `IOS_Ioctlv` (no HLE, `__ios_Ioctlv` -> IPC MMIO ->
   `IPCWriteReg` native = `abort()`), `0xCC003024` reset writes and an endless wait. New natives
   (`hle/os/os_quit.cpp`) make them exit cleanly. `OSFatal` now writes the crash artifact instead of
   drawing with the IPL font and spinning.
3. **`NANDCreate`/`NANDPrivateCreate` truncated existing files.** Sin's NAND library is the real guest
   code, so `ISFS_CreateFile` reaches `NAND_IOS_Ioctl_HLE` case CREATEFILE, which did `fopen("wb")` on an
   existing file (real IOS returns `FS_EEXIST` -105 and never truncates). Fixed (3.1).
4. Everything else in the NAND save path checks out statically (3.1).
5. No THP/BIK/movie anywhere (3.7), no CARD/MEM1 hardware use, IPL fonts only used by a debug caption
   object created at boot (3.8).

## 1. Methodology

* Active natives = `runtime/native-index.json` (285) + raw `REGISTER_NATIVE_FUNCTION*` (grep). Disabled =
  `SIN_DISABLED_NATIVE_OVERRIDE*` (expand to nothing; guest code runs).
* Names: `tools/sdk_name_map.txt`. It is fuzzy for a few entries (alias names at one address, e.g.
  `NANDSafeOpen`=0x801AA9F0 is interior of `__OSBootDol`; `OSResetSystem`=0x801AEA40 is the
  "obsolete" panic stub). Anything I rely on I re-read in the disassembly.
* Hardware-register toucher scan: any guest `lis 0xCC00/0xCD00..` outside GX FIFO/audio. Memory layer
  policy (`memory.cpp:615`): unmapped MMIO read/write **throws** (fatal) except VI regs (`ViMmio_Read*`).
  So a hardware toucher without native is a latent fatal, not a silent no-op.

## 2. Natives that are live at Sin addresses (non-GX/audio) and their status

| Function | Sin addr | Status | Evidence | Action |
|---|---|---|---|---|
| DVDReadPrio / ReadAsyncPrio / ReadAbsAsyncPrio | 8018D270 / 8018D180 / 80191410 | OK | game fs path: nw4r FileStream sets busy flag (stb 1,108) *before* `DVDReadAsyncPrio` (8020F5B8), HLE completes synchronously incl. callback, caller then reads result; state set to END | none |
| DVDLowInit/ReadDiskID/Inquiry/Read/Seek/UnencryptedRead/ClearCoverInterrupt | 801932E0.. | OK | real guest `DVDInit` (8018D480, HLE disabled) runs state machine over these | none |
| `DVDInit` donor HLE | 8015EA1C (donor) | disabled, body still runs via `DVDLowInit` | publishes FST/boot info, calls guest `__DVDFSInit` 8018CDA0 (correct) | donor `.bss` writes removed (0.1) |
| DVDGetDriveStatus 801916B0 (guest) | - | OK | reads FatalErrorFlag 804B202C, PausingFlag 804B2028, executing 804B20B0 (all 0, nothing sets them) -> returns END(0) | none |
| DVDClose/DVDCancel* (guest) | 8018D150/80191B20/801917B0 | OK | HLE sets cb state END (0) so `DVDCancelAsync` takes the "already done" branch | none |
| IOS_Open/Close/Read/Write, IOS_Ioctl | 80199D00/80199EF0/8019A0A0/8019A2B0/8019A5E0 | OK | `ISFS_*` guest wrappers call these (IOS_Read from 80199060, IOS_Write from 80199130) | none |
| IOS_OpenAsync/CloseAsync/ReadAsync/WriteAsync/SeekAsync/IoctlAsync/IoctlvAsync | 80199BE0..8019A850 | OK | arg regs match (r3 fd, r4 buf, r5 len, r6 cb, r7 arg); completion queued, pumped from `ProcessAlarmQueue` -> `NandProcessPendingCallbacks` | none |
| IOS_Ioctlv (sync) | 8019A940 | **no native** | 17 game/SDK callers (ESP_*, `__OSLaunchMenu`, `__OSRelaunchTitle`, `__OSInitPlayTime`) -> real `__ios_Ioctlv` -> `IPCWriteReg` native aborts | not reachable in normal gameplay (ESP_InitLib/GetDataDir/GetTitleId are native); quit paths now bypass it (os_quit) |
| NANDOpenAsync / NANDCloseAsync / NANDSeekAsync | 801A3EB0 / 801A40A0 / 801A33C0 | OK, **mixed layer** | signatures match (path,info,mode,cb,blk). Save writer 80113B24: NANDOpenAsync(HLE, shadow copy) -> NANDWriteAsync(guest -> IOS_WriteAsync HLE) -> NANDCloseAsync(HLE, atomic commit). Error path 801141D4 uses guest sync NANDClose = `CloseFd` (drops shadow = correct for an aborted save) | none; do not mix open-HLE with guest sync close on the success path |
| NANDDelete, nandMove (NANDMove 801A3720), NANDCheck | 801A2FD0 / 801A3570 / 801A5040 | OK | NANDMove HLE appends source name to dest *directory* like `nandMove`; only sync callers exist (801A3720) | none |
| NANDSafeOpen_HLE | 801AA9F0 | **bogus address** (interior of `__OSBootDol`) | name map alias | harmless; game does not use safe-open (no caller found) |
| PADInit/Reset/Recalibrate/Read/ControlMotor | 801B40B0/801B3E90/801B3FA0/801B4210/801B46D0 | OK | game pad reader 8002D208 calls these + guest `PADClampCircle` (pure math) | none |
| SIInit / SISetSamplingRate | 801B7AF0 / 801B8830 | OK (no-op) | remaining SI code unreachable: only reachable from PAD (all natives) | none |
| SCCheckStatus | 801B5010 | returns 0 always | OSInit/OSShutdownSystem spin on it | note: SYSCONF parse completes later via queued NAND callbacks; **UNVERIFIED** that it finished before first `SCGetLanguage` (result is English either way, see 3.5) |
| SCGetAspectRatio/EuRgb60Mode | 801B6900 / 801B69E0 | OK | callers 8003D880/8003DC58 (video setup) | none |
| SCGetBtDpdSensibility/WpadMotorMode/SensorBarPosition/SpeakerVolume, Get/SetBtDeviceInfoArray | 801B6C00..801B6BF0 | OK | verified by lead (M1) | none |
| SCGetProductArea / Code / SN / GameRegion | 801B7060 / 801B70F0 / 801B7130 / 801B71A0 | **was wrong** | tables/buffer were donor addresses | **fixed** (3.5) |
| OSGetTime / `__OSGetSystemTime` | 801B0CC0 / 801B0CF0 | OK | real OSGetTime = bare mftb; timebase 60.75 MHz (`timebase_contract.h`) | none |
| OSSetAlarm / OSSetPeriodicAlarm | 801A6F40 / 801A6FB0 | OK | Sin ABI: OSTime in r5:r6, handler r7 (disasm 801A6F58..) = what the HLE reads; periodic: r5:r6 start, r7:r8 period, r9 handler | none |
| OSSleepThread / OSSleepTicks, OS*Message*, SelectThread, context natives | 801B0900/801B0C10/801AD2E0..801AD4F0/801AFBB0/801A8790.. | OK | fixed earlier by lead (scheduler retarget) | none |
| OSLockMutex/UnlockMutex/WakeupThread/CreateThread/... | 801ADDA0.. | **guest** (donor HLEs disabled) | 84/92 game callers of Lock/Unlock | none; relies on guest + the retargeted scheduler |
| OSRegisterShutdownFunction | 801AE130 | stub (returns 1) | no shutdown callbacks ever run, fine because quit natives exit directly | none |
| OSGetResetCode, `__OSInitNet`, `__OSInitSTM`, PS init, OSGetConsoleType | 801AEA10 etc. | OK | boot-time | STM donor writes removed |
| OSInitFont | 801AC710 | blank stub | 3.8 | none |
| VIInit/`__VIInit`/Configure/Flush/SetNextFrameBuffer/SetBlack/WaitForRetrace/GetNextField/GetCurrentLine/GetDTVStatus/GetTvFormat/GetScanMode | 801BA9E0.. | OK | callbacks from guest slots 804B2410/0C (lead); VI MMIO reads only via natives | donor writes removed (0.1) |
| OSResetSystem | 801AEA40 | **misnamed** native | Sin 801AEA40 is the obsolete-API `OSPanic(__FILE__,1185,"...")` stub; only callers are `__check_pad3` 800062C0 and 801546C4 | harmless (clean exit instead of panic) |

### Raw (donor-address) registrations still present, all at *interior* addresses, none reachable

`REGISTER_NATIVE_FUNCTION*` at 0x801938FC (interior of DVDLowOpenPartition), 0x80169BCC (interior of
btm_remove_acl), 0x801A4EC4 (interior NANDInitBanner), 0x801A961C (interior PackArgs), 0x801D8D30
(interior GXInitTexObj), 0x8055531C/0x805B9344/0x8052F4E8/... (not code in Sin). They only fire if
something dispatches exactly that address. Recommended hygiene (not done: touches files outside the
"new files" rule): turn them into `SIN_DISABLED_*` / delete in the next retarget pass
(`os_init.cpp:29,37,82`, `nand_isfs.cpp:443,956`, `network_config.cpp:48`, `boot_race.cpp`).

## 3. Per-area detail

### 3.1 NAND / ISFS save path (save, quit, reload)

Layering in Sin: **guest** `nand.a` (NANDOpen/Read/Write/Close/Create/GetLength/GetStatus/InitBanner/
GetHomeDir...) over **guest** `fs.a` (ISFS_*) over HLE'd `IOS_*` (`nand_isfs.cpp`, `ios.cpp`). The map
names only 53 of nand.a (no NANDRead/Write/Create); I resolved them by call edges:

| Guest fn | Addr | Evidence |
|---|---|---|
| NANDCreate (sync) / NANDPrivateCreate / NANDCreateAsync | 801A2DE0 / 801A2CD0 / 801A2ED0 | -> ISFS_CreateFile 80198B70 / ISFS_CreateFileAsync 80198C70 |
| NANDRead (sync) | 801A31E0 | -> 80199060 (ISFS_Read) -> `IOS_Read` HLE |
| NANDWrite (sync) / NANDWriteAsync | 801A32D0 / 801A3340 | -> 80199130 / 80199150 -> `IOS_Write(Async)` HLE |
| NANDMove | 801A3720 | -> nandMove 801A3570 (HLE) |
| NANDGetStatus | 801A3AF0 | -> nandGetStatus -> ISFS_GetAttr |
| NANDGetLength | 801A3790 | -> ISFS_GetFileStats 80198F00 = ioctl 11 (HLE returns size,pos) |
| NANDOpen / NANDClose (sync) | 801A3D90 / 801A4030 | `NANDClose` requires `info+0x8A == 1`, writes `2`; HLE open writes the same flag |

Game save code (static, `tools/callees.py`): `801126C0` (create/validate all slots + banner:
NANDGetHomeDir, NANDOpen(banner), GetLength, NANDCheck(80112554), NANDDelete, NANDCreate,
NANDInitBanner, TPLBind/TPLGet for the banner icon, NANDOpen+NANDWrite(80112A6C)+NANDMove(80112ABC)),
`80112AEC` (delete slot: GetStatus, NANDDelete, NANDCreate), `80112C20` (sync save with OSGetTime stamp:
NANDOpen, NANDWrite, NANDClose), `801131E8` (sync load: NANDOpen, NANDRead(801132C4), NANDClose),
`80113B24` (async save state machine: NANDOpenAsync, NANDWriteAsync 1600 bytes = sdat size,
NANDCloseAsync), `8011314C/80114208` (NANDSetUserData/GetUserData).

Read-back verdict: **read path OK**. Sync load goes guest NANDOpen -> `ISFS_Open` -> `IOS_Open` HLE (mode
1 = "rb", ENOENT if missing, never creates) -> NANDRead -> `IOS_Read` HLE (`fread`) -> NANDClose ->
`IOS_Close` HLE. Home dir: `NANDGetHomeDir` guest -> `ESP_GetDataDir` native -> `.../title/00010004/52325645/data`
(directory exists on disk).

Bug fixed: `ISFS_IOCTL_CREATEFILE` truncated existing files (see 0.3). Known fidelity gaps left alone
(low risk, noted for later): `ISFS_IOCTL_GETATTR` fills perms/attr at the wrong labels
(`nand_isfs.cpp`, offsets 0x46..0x49 should be attr/owner/group/other; HLE puts attr=3 at 0x46 and
group perm=1/2 at 0x48), `NANDOpen_HLE` creates missing files on write-mode open (real: -12),
`ISFS_IOCTL_READDIR` via sync `IOS_Ioctlv` is unreachable (no sync Ioctlv HLE), async variant works.
Banner: `NANDInitBanner` (801A4EA0) is guest code operating on guest memory only; banner.bin on disk is 61600 bytes
(size produced by the game itself, not verified against hardware).

Files on disk after the lead's earlier runs: `sdat0..3.bin` 1600 bytes, `banner.bin` 61600 (sdat0
modified later than the others = a save write happened). That is circumstantial evidence the write path
(open/shadow/write/commit) worked end to end.

### 3.2 DVD

See table in section 2. Additional notes:
* Async completion is synchronous and *inside* the call. This is safe for the nw4r FileStream path
  (flag set before the call). **UNVERIFIED** for other async DVD users (8020F6D0 `DVDCancelAsync` caller,
  8027E580/8027EA70 `DVDGetDriveStatus` polling): static review shows no flag-after-call pattern, but
  there are only 4 direct game callers of the DVD API (all in the 0x8020F5xx/0x8026Dxxx fs layer).
* HLE read errors set state FATAL_ERROR and return -3 (real: -1 FATAL, -3 CANCELED); callers test `<0`.
* Stage swap/unload: stages are `.rso` files read with `DVDReadPrio` through the same path; M3 owns the
  module code-set swap. No DVD state is torn down on swap, nothing to fix.
* Cover/error states: `DVDLowGetCoverRegister`, `DVDLowPrepareCoverRegister` are guest and only reached from
  `__DVDGetCoverStatus` (quit paths, now bypassed). `__DVDShowFatalMessage` -> `OSFatal` (now native).

### 3.3 OS: time, alarms, threads, messages, semaphores

Alarms/threads/messages were retargeted earlier by the lead and match the Sin disassembly (ABI of
OSSetAlarm re-derived above). OSSemaphore is guest (80266030 `OSInitSemaphore`, built on mutex+cond).
Unaudited-because-dead code with donor constants (no native index entry): `os_thread.cpp` (disabled
CreateThread etc.), `task_thread.cpp` (THP/MKW), `os_alarm.cpp` RFL (`RFLiIsWorking`, disabled),
`network_deferred.cpp` default-thread constant (network only). They are inert unless re-enabled.

### 3.4 VI

No donor state remains active after 0.1. `VIGetScanMode` always reports interlace and `VIGetTvFormat` NTSC
(game checks `VIGetScanMode == VI_PROGRESSIVE` at 8003DD28 to pick 480p, so 480p is never selected: by design
for now). The DTV status read in `OSGetFontEncode` (lhz 0xCC00206E) goes through `ViMmio_Read16`.

### 3.5 SC / SYSCONF

R2VE01 tables (read from main.dol): area @0x8031F828 (5-byte rows `00 JPN, 01 USA, 02 EUR, 03 AUS, 04 BRA,
05 TWN, 05 ROC, 06 KOR, 07 HKG, 08 ASI, 09 LTN, 0A SAF, 0B CHN, FF`), game region @0x8031F870 (4-byte rows
`00 JP, 01 US, 02 EU, 04 KR, 05 CN, FF`), CODE buffer r13-21408 = 0x804B2360. The donor natives looked up
0x8029CEB0/0x8029CEF8 (Sin code) and wrote 0x803869E0 (.bss). Now retargeted in `sc.cpp` (body-only edit,
plain rebuild). Consumers: `SCGetProductArea` <- `SCGetLanguage` fallback + 80286C30 (WC24/online info,
`SCGetProductCode/SN`), `SCGetProductGameRegion` <- WUD (801CC1A0/801CE180/801CE6B0) - **this one may have
influenced Bluetooth setup before (returned -1)**. SYSCONF generated by `tools/gen_sysconf.py` has no CODE/
MODEL/SERNO/AREA items, which is fine because these four are native.
`SCGetLanguage` result (English) is the same with or without the fix.
Sound mode / aspect: `SCGetSoundMode` (guest, callers 801E1150 HBM, 8027B8C0) reads SYSCONF IPL.SND; the
generated file has no `IPL.SND` item -> the SDK fallback is used (**UNVERIFIED** which value; check the audio output mode if sound is mono/surround-wrong). `IPL.AR` = 0 is only used if the guest path reads
it; widescreen comes from the native `SCGetAspectRatio` (config switch).

### 3.6 PAD / KPAD / WPAD

GC path: all five PAD functions the game uses are native (801B4210 `PADRead` writes `PADStatus[4]` via
`PadStatusContract`). `PADSetSpec/AnalogMode` are never called by the game. The rest of PAD/SI is unreachable
(PAD callbacks `PADProbeCallback`.., `SIInterruptHandler`, `CompleteTransfer` have 0 callers outside PAD).
KPAD/WPAD owned by the lead (not touched).

### 3.7 THP / movies

`orig/R2VE01/files` contains only .arc (97), .brstm (139), .rso (8), .brfnt (2), .tpl (3), .brsar, .brtex,
.bnr, .csv, .sel, .txt: no .thp/.bik. No THP library in the SDK map (`libs` list has no `thp.a`).
Cutscenes are therefore in-engine/ARC. `task_thread.cpp`'s MKW THP code is dead for Sin.

### 3.8 Fonts

Callers of the IPL font API: `OSInitFont` and `OSGetFontEncode` from `fn_8020FE40` (a debug caption
object constructed in main at 8003DAA4 -> `fn_800271BC`), `OSGetFontTexture` (802101F4) and `OSGetFontWidth`
(8021011C) have no static callers (vtable/debug only). All real text comes from `.brfnt`/ARC fonts read from
disc. Verdict: the blank `OSInitFont` stub is fine for gameplay; if debug text ever shows, it will be blank.
The title screen "missing press-start text" is therefore not an IPL font problem.

### 3.9 Quit / reset / HOME menu

Game flow (static): `main` 8003D9C8 runs the frame function 8003EC1C; on exit it checks flags at
object+13/+14/+15 and calls `OSRestart(0)` / `OSShutdownSystem()` / `OSReturnToDataManager()`; 8003EC1C itself
calls `OSReturnToMenu()` (8003EE1C) and `OSRestart` (8003EE3C) after an `OSReport`. New natives
(`hle/os/os_quit.cpp`) at 801AE3F0 / 801AE510 / 801AE880 / 801AE8C0 stop GX+AX workers, `Memory::Reset()`,
exit code 0 (same sequence as the existing `OSResetSystem` native). NAND writes are flushed per write
(`fflush` in `IOS_Write`) and committed at `NANDClose(Async)`, so no extra flush is needed on exit.
`OSFatal` (801AAF80) logs, writes crash artifacts and exits non-zero. No `OSSetResetCallback`/reset-switch
API exists in this SDK build (name map has none), and `OSSetPowerCallback` is never reached (donor HLE
disabled; guest registration goes nowhere since no STM events are ever delivered).
Behaviour change to be aware of: **Reset from the HOME menu now exits the process** instead of relaunching the
title (a console would restart the game). Relaunch is a deliberate non-goal here.

## 4. Hardware-register touchers without native (outside GX FIFO and audio)

From `tools/hle_retarget_report.py --hw`. All are either unreachable from the game, or only reachable
through quit paths that are now native.

| Function | Addr | Reachable? |
|---|---|---|
| SI: CompleteTransfer, SIInterruptHandler, `__SITransfer`, SIGetStatus, SISetCommand, SITransferCommands, SISetXY, SIEnable/DisablePolling, SIGetResponse | 801B7290..801B7F70 | only from PAD (natives) / SIInit (native) |
| EXI: `__EXIProbe`, EXIAttach, EXIGetID, EXI/TC/EXT interrupt handlers | 801967C0.. | only via `__OSEnableBarnacle` <- EXIInit (native) |
| `__OSGetDIConfig` | 801A6BB0 | called from OSInit region only (boot passes) |
| `__OSDispatchInterrupt`, MEMIntrruptHandler, OnShutdown, BATConfig, `__OSUnhandledException` | 801ACFC0.. | exception/boot/shutdown only |
| `__OSBootDolSimple`, `__OSShutdownToSBY`, `__OSHotResetForPDN`, `__OSStateEventHandler`, ConfigureVideo, OSFatal | 801AA1F0.. | quit paths (now native) |
| `__VIRetraceHandler`, `__VISetRGBModeImm`, I2C helpers (`sendSlaveAddr`...) | 801BA010.. | never called (VI is native; retrace is emulated) |
| `__GXShutdown`, `GXAbortFrame` (801D6D80, game 8003F088) | - | GX agent (CC00 reads are PE/ CP) |
| 80158AA8, 80158DB8..80159464 (CD00 AI/ARQ helpers), `__AIDHandler`, `__AI_SRC_INIT`, `__DSPHandler` | - | audio agent |

## 5. Edits made by this audit

New: `runtime/src/hle/os/os_quit.cpp` (5 natives: OSShutdownSystem 801AE3F0, OSRestart 801AE510,
OSReturnToMenu 801AE880, OSReturnToDataManager 801AE8C0, OSFatal 801AAF80), `tools/hle_retarget_report.py`,
`tools/hle_callers.py`, `tools/dis_dol.py`, this doc.

Edited (minimal, existing files, none of the forbidden list):

* `hle/vi.cpp` - drop 5 donor-address writes in `WriteGuestStateLocked`.
* `hle/storage/dvd.cpp` - `CompleteDvdCancelState` now empty; donor flag/context writes removed from `DVDInit_8015EA1C`.
* `hle/os/os_interrupt.cpp` - IPCCltInit: donor buffer-lo write removed.
* `hle/os/os_init.cpp` - `__OSInitSTM` donor writes removed.
* `hle/sc.cpp` - SC product natives retargeted to Sin tables/buffer (no registration change).
* `hle/storage/nand_isfs.cpp` - CREATEFILE returns -105 for existing entries instead of truncating.

Build: the new natives add registrations, so run the **full chain** (PLAN-NEXT section 2: emit-native-index,
translate-recursive, generate-data-init, emit-build-shards, then cmake). The other edits are body-only.

## 6. Verification steps for the lead

1. Full chain build, then boot to title as before (`tools/run_game.ps1 -Seconds 60 -Name hle1 -Env "SIN_VIRTUAL_REMOTE=1"`).
   Expect no new `FATAL`/`crash_*.txt`, and the DVD read log lines unchanged.
2. **Corruption check (cheap, deterministic):** with `SIN_WATCHDOG_SECS=20` take the `mem1.bin` of the previous
   build and this build at the same point and diff the ranges from section 0.1
   (`0x80343230..0x80343250`, `0x8038667C`, `0x803866A8`, `0x80350864..0x8035087E`, `0x80386B44`, `0x803867EC`,
   `0x80386934..0x8038693F`, `0x803869E0`); they should stay zero/untouched now. Any visual change on the
   title screen after this build is worth a note (a donor write used to hit game data).
3. **Quit test:** `SIN_VREMOTE_SCRIPT="30;1:HOME;5"` (30 s idle, then Home pulse) should open the HOME menu; choosing
   "Wii Menu"/"Reset" with the stick/A must end the process with exit code 0 and the log lines
   `OSReturnToMenu: guest requested exit...` or `OSRestart(...)`. Without HBM navigation you can force the same
   path with `SIN_TRACE_ARGS=801AE880+801AE510` to confirm the target ran.
4. **Save -> quit -> reload** (needs the game's own save point; I could not derive a button script for it):
   1. `Copy-Item build\native\UserData\NAND\title\00010004\52325645\data build\nand_before -Recurse`
   2. Run with `SIN_CALLTRACE=1 SIN_TRACE_ARGS=801A3340+801A32D0+801A3D90+801A3EB0+801A2DE0+801A2CD0` (logs args of
      NANDWriteAsync, NANDWrite, NANDOpen, NANDOpenAsync, NANDCreate, NANDPrivateCreate). Do the in-game save
      (options change or stage clear; slot data is 1600 bytes), then quit via HOME -> Wii Menu.
   3. `Get-FileHash build\native\UserData\NAND\title\00010004\52325645\data\sdat*.bin` vs `build\nand_before`:
      exactly the written slot must change, size must stay 1600 (`banner.bin` 61600), and no
      `*.nandsafe.tmp` file may remain in that folder (a leftover = a shadow handle was never committed).
   4. Relaunch: the log must show NANDOpen (801A3D90/801A3EB0) then `NANDRead` (801A31E0 args) for that slot and the
      menu must show the saved progress. A `[nand]` ERROR line, or `sdat*.bin` size 0, is a failure
      (the old CREATEFILE bug produced exactly that).
5. If an Xbox-controller test of the HOME item is awkward: `SIN_VREMOTE_SCRIPT` buttons are `A B 1 2 PLUS MINUS
   HOME UP DOWN LEFT RIGHT`, segments separated by `;`, pairs by `,` (see `virtual_remote.cpp:277`).

## 7. Risks

* The `CompleteDvdCancelState` removal assumes no guest code *depends* on the HLE having zeroed
  Canceling/executing. Statically nothing does (HLE reads never enter the guest queue), but a first run should
  confirm DVD reads still complete (they did before only by not touching real state).
* `os_quit.cpp` ends the process on any guest call to those functions; a spurious call would silently quit.
  The log line names which one fired.
* GETATTR layout and NANDOpen write-create differences are unfixed fidelity gaps (3.1).
* `SCCheckStatus` returning 0 early (3.5) is unchanged; SYSCONF parsing may finish late.

# Sin & Punishment recomp - audio notes (static analysis, 2026-11-20)

Status: **UNVERIFIED at runtime.** Everything below comes from reading translated bodies
(`generated/functions/func_*.cpp`), `tools/sdk_name_map.txt`, `tools/sdk_globals_map.txt` and old run logs. Nothing
here was built or run (the lead owns builds). The fixes below remove the reasons audio could not work; they
do not prove it does. Tool: `python tools/audio_retarget_report.py [--all] [--no-hw]`.

## 1. How Sin's audio is built (what runs where)

* Sin links the 2009 RVL SDK AX (`<< RVL_SDK - AX release build: Feb 27 2009 (0x4302_145) >>`, banner seen in the boot
  log), MIX, nw4r snd (`libnw4r_snd.a`, 149 mapped functions), HBM (home button) sound, and a *stub-only* `axfx.a`
  (0x8015D840..0x8015D8F0, i.e. the game does not use AXFX reverb).
* No ARAM/ARQ code exists in Sin (`tools/sdk_name_map.txt` has no AR*/ARQ entries besides ARC archive helpers; the only
  guest code touching the ARAM DMA registers 0xCC005020.. is `__OSInitAudioSystem` 0x801A7BD0,
  `__OSStopAudioSystem` 0x801A7DA0 and the exception dumper `__OSUnhandledException` 0x801A9250). Wii AX reads sample
  data straight from MEM1/MEM2, which the HLE mixer already does (`ax_memory.cpp` `AramAliasesFor`).
* Music streaming is **not** AI/DVD-audio streaming. `stream\*.brstm` is read through the DVD HLE
  (`[dvd] read ...\stream\title.brstm off=.. len=0x4000` in the console log) by guest nw4r code
  (`StrmSound` / `StrmPlayer` / ut::DvdFileStream, translated, running on its own threads); the PCM/ADPCM ring buffers sit in
  main RAM and are played by AX voices. There are no AIS (`AIRegisterStreamCallback` ...) functions in Sin's `ai.a`.
* No THP symbols exist in the name map (only an unrelated `__HBMSYNMidiIn`), so movie audio is not a concern unless the game
  uses its own player.
* The real SDK AX code (AXInit, __AXOutInit, __AXOutAiCallback, __AXOutNewFrame, __AXSyncPBs, AXAcquireVoice,
  AXSetVoice*, __AXServiceVPB ...) and all of nw4r snd run as *translated guest code*. Only the hardware boundary
  is HLE: AI DMA, the DSP mailbox/task layer and the DSP microcode itself (a Dolphin-derived AX command-list mixer).

### Frame pipeline (all addresses Sin / R2VE01, verified in translated bodies)

```
OSInit -> __OSInitAudioSystem 0x801A7BD0 (native)  -> AxDspHle::InitAram/Init
game main -> AXInit 0x8015A4E0 (guest) -> __AXOutInit 0x8015C260 (guest)
   -> __AXOutInitDSP 0x8015C170 (NATIVE: fills __AXDSPTask 0x803D89A0, sets __AXDSPInitFlag)
   -> AIRegisterDMACallback 0x80159550 (NATIVE, new) (__AXOutAiCallback 0x8015C030)
   -> AIInitDMA 0x801595A0 (buf 0x803D8520, 384 B = 96 stereo samples = 3 ms @ 32 kHz) / AIStartDMA 0x80159620 (natives)
every poll (scheduler idle loop, VI retrace, main.cpp): Audio_HLE_Tick (audio.cpp)
   accumulate wall time -> per 3 ms block: join mix worker, PushAudioBlock -> AudioBackend (SDL3 stream, 32 kHz S16 stereo),
   then InvokeIndirectCpu(__AXOutAiCallback) -> guest __AXOutNewFrame builds the command list
   -> DSPSendMailToDSP 0x8018C290 (native) -> AxDspHle HandleMail/DispatchCommandList (+ mix worker)
   -> AxDspHle::ServiceDeferredCallbacks -> guest __AXDSPResumeCallback 0x8015C0F0 ...
```

The DSP ucode in Sin (`axDspSlave` 0x80307180, length 0x2000, init vector 0x0010, resume 0x0037) has the
HashEctor CRC **0x4CC52064** (computed from `orig/R2VE01/sys/main.dol` with the same rotate-xor hash
`HashEctorGuest` uses). That is one of the recognised ucodes in `ax_internal.h` (`PBLayout::SkipUpdatesOnly`,
new filter, new command layout), so the mixer takes the known-good path, not the auto-detect fallback.

## 2. What was wrong (root causes found)

1. **`AIRegisterDMACallback` had no native at the Sin address.** The donor native was
   `SIN_DISABLED_NATIVE_OVERRIDE(80123f88...)`, so the *guest* function ran and stored `__AXOutAiCallback` in its own
   `__AID_Callback` (0x804B1E40). The HLE tick (`g_ai.callback`) only learns the callback through this native, so it
   stayed 0 and `Audio_HLE_Tick` silently skipped the AI callback: **no AX frame was ever produced, no voice ever
   mixed, the DMA buffer stayed all zeros** (and any guest code waiting for AX frames - nw4r SoundThread - would just
   wait). This alone explains "audible output unverified".
2. **Donor (MKWii) data addresses in the audio HLE corrupted Sin memory.** `audio.cpp`/`ax_*.cpp` wrote/read
   0x80386448/4C/7C/80, 0x80386608..0x80386624, 0x802F81A0 (a 0x3C-byte DSPTask), 0x802F8200, 0x80385800/02/04,
   0x80386524/28 in Sin's data/bss every AI/DSP/AX call, and *never* set the flags the Sin SDK reads
   (`__AI_init_flag`, `__DSP_init_flag`, `__AXDSPInitFlag`...). All retargeted (table below).
3. **Wrong task callbacks.** `__AXOutInitDSP` HLE stored donor callbacks at task+0x28..0x34
   (`0x80126948`, `0x8015C0F0`(coincidentally right), `0x8016000C`, `0x801269B8`); Sin's are
   0x8015C0E0 / 0x8015C0F0 / 0x8015C150 / 0x8015C160 (decoded from `lis 0x8016, addi -16160/-16144/-16048/-16032`).
   Note `tools/sdk_name_map.txt` / `retarget_resolved.json` labels 0x8016000C `__AXDSPRequestCallback`: that is
   **wrong**; the request callback is 0x8015C160.
4. **`MusicAttenuation::TickGuest()`** (called from `Audio_HLE_Poll`, i.e. every audio poll) walks MKWii's sound-manager
   graph at donor address 0x809C2898 and *writes* volume floats through whatever it resolves to. In Sin that address is an
   unrelated heap location, so it was a latent memory-corruption source. Disabled (see below).
5. `ax_effects.cpp` registered a native on 0x8012B830 (MKWii `AXFXReverbStdExpCallback`). No Sin function exists there and
   Sin doesn't use AXFX. Registration commented out.

## 3. Natives: status at Sin addresses

| Sin addr | Function | State | Evidence |
|---|---|---|---|
| 0x801A79B0 | `__AIClockInit` | enabled (was already) | name map diff=0; only touches 0xCD800180.. clock regs |
| 0x801A7BD0 | `__OSInitAudioSystem` | enabled | name map; body: DSP boot-code copy via ARAM DMA regs |
| 0x801A7DA0 | `__OSStopAudioSystem` | enabled | name map; writes 0xCC005036/0xCC00500A |
| 0x80159690 | `AIInit` | enabled, **globals retargeted** | body writes r13-22776 (init flag), -22724 (stack), -22720 (cb) |
| 0x80159550 | `AIRegisterDMACallback` | **ENABLED NOW (new)** | body: `old=lwz -22720(r13); stw r3,-22720(r13); return old` between OSDisable/RestoreInterrupts; callers 8015C260, 80266410, 801B2CA0 |
| 0x801595A0 | `AIInitDMA` | enabled | body writes 0xCC005030/32/36 |
| 0x80159620 | `AIStartDMA` | enabled | body sets bit 15 of 0xCC005036 |
| 0x80159640/50/70 | `AIGetDMABytesLeft/StartAddr/Length` | enabled | bodies read 0xCC00503A / 5030,5032 / 5036 |
| 0x80159680 | `AICheckInit` | real code (no native needed) | `lwz r3,-22776(r13)`; AIInit native now writes that flag at the Sin address |
| 0x8018C2B0 | `DSPInit` | enabled | writes __DSP_* globals + DSP ctrl |
| 0x8018C250/60/70/90 | `DSPCheckMailToDSP/FromDSP/ReadMailFromDSP/SendMailToDSP` | enabled | 0xCC005000/04/06 mailbox regs |
| 0x8018C380 / C3F0 / CAE0 | `DSPAddTask` / `DSPAssertTask` / `__DSP_boot_task` | enabled, **globals retargeted** | bodies use r13-22328..-22300 (0x804B1FC8..FE4) |
| 0x8018C370 | `DSPCheckInit` | real code (leaf, inlined in __AXOutInitDSP) | reads r13-22328 = `__DSP_init_flag`; MarkDspInitialized writes it |
| 0x8015C170 | `__AXOutInitDSP` | enabled, **rewritten for Sin** | full body decoded; sets task, flags, OSInitThreadQueue |
| 0x80159810 `__AIDHandler`, 0x8018C500 `__DSPHandler`, 0x80159930 `__AI_SRC_INIT`, 0x801ACFC0 `__OSDispatchInterrupt` | | no native, **not needed**: only reachable from the real AIInit/DSPInit/interrupt exception path, all replaced by HLE (interrupts are simulated) | |
| 0x801A9250 `__OSUnhandledException` | | no native, only runs on crash dump | |

Disabled donor leftovers in `audio.cpp`: `AIGetDSPSampleRate` (Sin's ai.a has none), `SoundPlayerSetVolume` (MKWii nw4r
SoundPlayer::SetVolume at donor 0x800A35E0; needed only by the in-game music/sfx volume sliders), `AICheckInit`/`DSPCheckInit` raw registrations (commented).

### Constants retargeted (donor -> Sin), all from translated bodies

| Meaning | Donor | Sin |
|---|---|---|
| `__AI_init_flag` / `__AID_Active` / `__CallbackStack` / `__AID_Callback` | 80386448 / 4C / 7C / 80 | 804B1E08 / 1E0C / 1E3C / 1E40 |
| `__DSP_init_flag`, rude_pending, rude_task, tmp_task, last_task (tail), first_task, curr_task | 80386608, 10, 14, 18, 1C, 20, 24 | 804B1FC8, 1FD0, 1FD4, 1FD8, 1FDC, 1FE0, 1FE4 |
| `__AXDSPTask` (AX DSPTask, 0x3C+ bytes) | 802F81A0 | **803D89A0** |
| IRAM image `axDspSlave` / DRAM image | 8027F820 / 802F8200 | 80307180 / 803D8A00 |
| task init/resume/done/request callbacks | 80126948/8015C0F0/8016000C/801269B8 | 8015C0E0 / 8015C0F0 / 8015C150 / 8015C160 |
| axDspInitVector/ResumeVector/SlaveLength (u16) | 80385800/02/04 | 804B0990/0992/0994 |
| `__AXDSPDoneFlag` / `__AXDSPInitFlag` | 80386524 / 80386528 | 804B1EEC / 804B1EF0 |
| `__AXOutThreadQueue` (new write) | - | 804B1EE0 |

## 4. Output path (does the pipeline work for Sin?)

AudioBackend (`runtime/src/audio_backend.cpp`) opens an SDL3 default-playback stream (S16LE stereo @ 32000 Hz,
`SDL_OpenAudioDeviceStream`, resampled by SDL). `PushWiiAiSamplesBE16` swaps the AI's R,L big-endian frames. The "audio mix worker"
option (`RuntimeConfigFile::AudioMixWorkerEnabled`, default true, toggle in the settings overlay) runs the command-list mix on a
thread; the tick joins it before pushing the block and before the guest callback, so it is correctness-neutral.
Nothing in the pipeline is Sin-specific once the addresses above are right. Timing: blocks are claimed from wall-clock
time at 3 ms each, max 4 per poll; the scheduler idle loop polls continuously, the VI retrace poll alone could only deliver 12 of 16.7 ms,
so **audio relies on the idle loop being reached** (main thread blocks on the retrace queue each frame, so it is).

## 5. Debug: SIN_AUDIO_LOG=1

New `runtime/src/hle/audio/audio_debug.h`. With `SIN_AUDIO_LOG=1` the console log (stderr) gets, low rate:

```
[audio-dbg] __AXOutInitDSP task=0x803D89A0 iram=0x80307180 len=0x2000 initVec=0x0010 resumeVec=0x0037 ucodeCrc=0x4CC52064
[audio-dbg] voice START pb=0x........ rate=32000Hz vol=7FFF/7FFF fmt=0x.... src=0     (first 160 transitions only)
[audio-dbg] voice STOP  pb=0x........ ...
[audio-dbg] 1s: aiBlocks=+333 axFrames=+333 aiCallbacks=+333 nonSilentBlocks=+N voices=K (started S stopped T) peak=P rate=32000Hz dma=1 cb=1
```

`rate` of a voice = SRC ratio (16.16) x 32 kHz. `peak` is the max |sample| of the PCM actually handed to SDL since the last line.

## 6. Verification steps for the lead

1. Full rebuild chain (registrations changed: **added** 0x80159550, **removed** 0x8012B830 - `emit-native-index` must be re-run):
   `emit-native-index` -> `translate-recursive` -> `generate-data-init` -> `emit-build-shards` -> cmake build (PLAN-NEXT section 2).
   Check `runtime/native-index.json` contains `0x80159550 AIRegisterDMACallback_80123f88` and no `0x8012B830`.
2. Run: `powershell -File tools\run_game.ps1 -Seconds 60 -Name aud -Env SIN_AUDIO_LOG=1,SIN_VIRTUAL_REMOTE=1` and
   `Select-String build\native\UserData\Logs\base_*\console.log -Pattern 'audio'`.
3. PASS evidence, in order:
   * `[audio-dbg] __AXOutInitDSP ... ucodeCrc=0x4CC52064` (0 or another value = the ucode bytes at 0x80307180 are not what was expected).
   * `[audio-dbg] 1s:` lines with `aiBlocks=+~333` AND `aiCallbacks=+~333` AND `axFrames=+~333` (if aiCallbacks stays 0 the
     AIRegisterDMACallback native is not live: stale native-index/shards; if aiCallbacks>0 but axFrames=0, the
     guest never sent the command list - look for a stall in `__AXOutNewFrame` 0x8015BDD0 / `DSPSendMailToDSP`).
   * `voice START` lines around title/BGM start (title.brstm is streamed right after boot) and `voices=` > 0.
   * `peak` > 0 and `nonSilentBlocks` > 0 = real PCM reached SDL. Then listen: the title BGM should be audible.
   * No `[audio] AI DMA callback not registered; skipping`, no `[audio] worker aborted a command list`, no
     `unknown AX command`, no `worker skipped a guest access outside MEM1/MEM2`.
4. If aiBlocks is ~333/s but audio is crackling: check that `aiBlocks` is not capped (poll rate) and whether
   `kMaxBlocksPerTick` backlog is growing; try the mix-worker toggle.

## 7. Remaining unverified / not done

* Everything runtime: AX command-list mixing correctness for Sin's PB layout, ADPCM/PCM16 voices, Sin's nw4r sound-mode
  (stereo/surround/DPL2), the aux buses (no AXFX, but nw4r uses AUX A/B/C callbacks: `__AXProcessAux` is guest code),
  compressor, voice priority/stealing - all rely on the donor mixer.
* Wii Remote speaker audio (`AXRmt*`, WPAD stream data) - AXRmtAdvancePtr is called; whether BT HLE accepts the
  speaker data is not audio-output relevant and untouched.
* In-game volume sliders (music/SFX/UI/voice) in `settings_overlay.cpp` drive `MusicAttenuation`, which is MKWii-specific
  (donor SoundPlayer layout at 0x809C2898, SetVolume donor 0x800A35E0): they have no effect in Sin. Master volume/mute does work
  (SDL gain). Porting needs the Sin nw4r `SoundPlayer` struct (`nw4r::snd::SoundPlayer`) offsets.
* `hle/boot_race.cpp` still has `KartAIController_IsCPU` at 0x8072624C (donor, not audio).
* Sample rate of the backend is hardcoded 32 kHz (matches Wii AX); AIGetDSPSampleRate does not exist in Sin so no mode switch.
* The wall-clock driven 3 ms block scheduling (instead of the DSP's 4 kHz timer) can drift against the game's own frame pacing
  under load; if audio stutters, the cap constants in `audio.cpp` are the knobs.

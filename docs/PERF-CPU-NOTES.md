# Perf: guest CPU hot paths - status when the agent ran out of usage

Agent stopped mid-task (usage). Partial work salvaged by the lead. NOTHING below is conclusive.

## Instrumentation added (in tree, unverified in a clean build)

- `runtime/src/hle/prof_sampler.cpp`: `SIN_PROF_SAMPLER=1` host-RIP sampling profiler of the guest thread,
  writes `prof_samples.txt` every 10 s; `tools/prof_resolve.py` maps addresses to symbols with llvm-nm.
- `[perf-gf]` line (perf_log.cpp + gx_copy.cpp): per-second guest-frame wall ms and guest-thread Mcycles/frame
  percentiles. `[perf-pacing]` line from the IO agent.
- One code change in `runtime/include/guest_abi/dispatch.h`: `MaybeServicePeriodicAlarms` uses a relaxed
  load/store instead of a locked fetch_add on every bridged call.

## A/B results (stage 1 = autostage 1, stage 6 = autostage 6, 140 s scripted runs, warm)

| Run (exe) | Stage | wall p50/p99 ms | cpu Mcyc p50/p99 per frame |
|---|---|---|---|
| base (`WiiCompiled-cpubase`, pre-edit) | 1 | 16.68 / 19.77 | 7.1 / 8.3 |
| same exe re-run (ab_ctl) | 1 | 16.69 / 17.22 | 6.1 / 6.7 |
| new build (alarm relaxed-store) | 1 | 16.71 / 17.69 | 7.4 / 8.5 |
| PGO-trained (`sin.profdata`) | 1 | 16.63 / 17.70 | 6.3 / 7.6 |
| control | 6 | 16.59 / 19.38 | 10.5 / 12.7 |
| PGO-trained | 6 | 16.57 / 19.12 | 9.3 / 10.9 |

**Run-to-run variance is ~14% (7.1 vs 6.1 Mcyc on the same exe).** Conclusions:

- The alarm relaxed-store change: no proven benefit (p50 slightly worse, p99 wall slightly better; within noise).
  Semantically safe; keep or revert after a cleaner A/B (more runs, interleaved order).
- PGO: stage 6 suggests ~11% less CPU per frame, stage 1 none. Not conclusive at this variance. The trained
  profile is at `build/tmp_cpu/sin.profdata` (9.3 MB, scratch; regenerate with the PGO commands in the agent
  scripts `build/tmp_cpu/pgobuild.ps1` before relying on it).
- Guest frame p50 is 16.6-16.7 ms with p99 ~17-20 ms: the guest thread is NOT the source of the 1% dips at 60 fps.

## Not done (take over here)

- Hotspot ranking from the sampler (`prof6.log`/`prof6b.log` + `tools/prof_resolve.py` exist; never analysed).
- Guest-thread priority/affinity (E-cores), page prefaulting, further codegen changes, PGO verdict.

Scratch: `build/tmp_cpu/` (logs + profdata). Delete after the analysis is extracted.

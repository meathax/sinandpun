# Busy-scene GPU performance

On an RTX 4090, the optimized ray-traced lighting pass took about half the GPU time in two fixed gameplay captures. Live guest frame rates varied by stage.

## Campaign measurements

The original eight campaign captures ranked Stage 5, Stage 4 and Stage 0 highest by peak GX draws per frame averaged over 600 frames after the stage module loaded (332, 252 and 241 respectively). This ranks the captured routes by draw density. Other routes, bosses and checkpoints can have different bottlenecks.

The following measurements use a 3840 × 2160 D3D12 output, upscaling off, 1× MSAA, ray-traced GI, strong bounce, retained geometry, muted audio and original guest pacing. The game chose the internal scene dimensions. The table covers guest frames 2400–8400 on Stages 4 and 5 and 2400–6000 on Stage 0.

| Stage and order | Mean guest FPS | Frames above 25 ms | Typical p99 (ms) | Worst interval (ms) | GPU trace (ms) |
|---|---:|---:|---:|---:|---:|
| 5, reference first | 59.20 → 59.86 | 76 → 13 | 19.43 → 20.04 | 80.31 → 71.16 | 1.60 → 1.15 |
| 5, optimized first | 59.49 → 59.67 | 49 → 32 | 19.78 → 20.09 | 79.61 → 67.90 | 1.87 → 1.23 |
| 4 | 59.85 → 59.74 | 15 → 23 | 19.66 → 18.87 | 38.33 → 49.79 | 2.40 → 1.43 |
| 0 | 54.53 → 54.49 | 362 → 364 | 19.45 → 18.50 | 37.82 → 38.49 | 2.85 → 1.28 |

Each arrow goes from reference to optimized. FPS is the mean of one-second guest FPS readings. P99 is the median of one-second p99 guest intervals. The slow-frame count sums measured intervals above 25 ms and includes deliberate changes in cadence. Stage 0 ran at approximately 30 guest FPS for part of its demo sequence while VI time advanced at normal speed in both builds.

## Draw-heavy windows

For each stage, the table shows the same 600-frame interval in both builds. It was selected by the higher shared draw load, using the smaller draw count of the two runs to rank each interval. The scripted controller uses wall time, so draw counts and individual game events can differ.

| Stage | Guest frames | GX draws/frame | Mean guest FPS | Frames above 25 ms | GPU trace (ms) |
|---|---:|---:|---:|---:|---:|
| 5 | 6000–6600 | 358 → 376 | 58.90 → 59.80 | 12 → 1 | 1.64 → 1.21 |
| 4 | 5400–6000 | 245 → 254 | 60.00 → 58.81 | 0 → 12 | 2.24 → 1.39 |
| 0 | 3600–4200 | 243 → 241 | 60.00 → 60.01 | 0 → 0 | 1.91 → 0.69 |

## GPU and CPU changes

The lighting shader traces two rays per receiver instead of four. Each receiver still samples both elevation bands. Neighboring cells alternate the horizontal direction, and the existing depth-aware spatial filter combines those samples. The pattern stays fixed over time. This changes indirect light slightly.

| Fixed capture | GPU trace time | Reduction | Mean absolute RGB difference (0–255) | Maximum channel difference |
|---|---:|---:|---:|---:|
| Combat | 1.169 → 0.642 ms | 45.1% | 0.055 | 7 |
| Stage 0 | 1.209 → 0.639 ms | 47.2% | 0.047 | 5 |

These figures use Vulkan GPU timestamps on the same 4090, with 80 timed renders per build and capture after warmup, in reference/optimized/optimized/reference order. The final composites preserved alpha exactly. The 99th-percentile color-channel difference was one level in both scenes. Two static captures cannot establish image quality for every moving scene.

The CPU now records only newly staged texture uploads per render operation. The render worker references the immutable recorded operation directly. This avoids repeatedly allocating and copying the growing upload list while preserving FIFO upload order. Full-frame replay still constructs the complete upload list. CPU bookkeeping was measured together with the shader change in live play.

## Reproduction and scope

Both release executables were linked against the same checkout and libraries, including concurrent workspace changes. The reference overrides the three changed translation units with the previous implementations of the affected functions. Runs used separate data directories seeded from the same stage-specific cache, save and controller script. Stage 5 was repeated in reverse build order. Machine activity and wall-clock input timing can affect live comparisons. No generated presentation frames contribute to the guest FPS figures.

The local evidence is under `build/qa/busy-fps/`: `stress.py` and `repeat.py` drive gameplay, `pair.py` constructs the reference, `metrics.json` contains the per-window results, and `final-gpu.py` plus the two `final-wall-gi-*.txt` files reproduce the fixed-capture comparison. The tested game is `build/clean/BusyCandidate.exe`.

## Stage 5 throughput above 60 FPS

The original pacing tests measure missed deadlines during normal play. To measure headroom above 60 FPS, a second pair of release builds used a benchmark-only 480 Hz virtual retrace and rendered the final 4K output to an offscreen GPU texture. This kept the final rendering pass in the workload and removed swapchain acquisition, presentation and monitor pacing. The release code retains original guest timing. The test used fixed controller input after stage entry and counted guest-generated frames.

| Window | Mean guest FPS, reference → optimized | Median GPU trace, reference → optimized |
|---|---:|---:|
| Stage 5 frames 4800–5400, busiest shared draw window | 109.50 → 119.88 (+9.5%) | 1.285 → 0.910 ms |
| Stage 5 frames 2400–8400, mixed activity | 143.07 → 145.78 (+1.9%) | 1.020 → 0.655 ms |

The busiest window averaged 348 versus 346 GX draws per frame. Its median one-second p99 fell from 12.22 to 10.83 ms and its longest observed interval fell from 40.73 to 12.15 ms. This was one pass per build. Script and guest timing can shift individual events. Late light-load samples reached about 478 FPS in both builds, close to the benchmark retrace ceiling. The mixed-window mean includes those samples and is the unweighted mean of the one-second FPS readings.

The offscreen comparison uses `off-rate-pair.py`, `off-rate-run.py`, `off-before-5/err.log`, `off-after-5/err.log` and `off-rate-summary.py` under `build/qa/busy-fps/`.

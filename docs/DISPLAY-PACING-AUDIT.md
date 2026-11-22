# Windows display pacing audit

The November 22, 2026 test used an RTX 4090, D3D12, a 120 Hz desktop and scripted Stage 1 input. The runtime counts calls to present. Physical scanout, panel VRR engagement, end-to-end latency and controller feel require hardware measurement or hands-on testing.

| Setting | Observed behavior |
| --- | --- |
| Original speed | About 60 guest updates and 60 presents per second in the baseline run. |
| Uncapped, VSync off, DLSS Quality | About 60 guest updates and 120 presents per second during active rendering. Some scenes returned to 60 presents. |
| Uncapped with an 80 FPS ceiling | The final four one-second samples reported 74, 72, 73 and 71 presents. |
| 120 FPS, VSync on, DLAA | About 120 presents per second at 60 guest updates per second. |
| Match display, automatic VRR limiter | The runtime selected 117 FPS and reported 116.9 presents per second in each of the final four samples. |

Changing the FPS ceiling now updates the renderer immediately. The retained-scene path observes that ceiling, and the settings graph reads actual present intervals. The full-run guest timing logs include startup stalls over 200 ms, so the steady gameplay rates above should not be read as worst-case guarantees.

DLSS Quality and native-resolution DLAA both reached the NVIDIA evaluation path. Reflex reported low-latency mode and game-controlled sleep enabled in driver readback.

The subsequent RTX 4090 test used DLSS Quality at a 3840 × 2160 output with NVIDIA Frame Generation enabled. In the focused game, Streamline reported success, status zero and two actual presents per application frame. Repeated measurement windows contained 240 NVIDIA presents for 120 application frames. With the game unfocused the plugin suspended interpolation. In the off comparison the ordinary presenter ran with Reflex still active. Driver readback reported low-latency mode and game-controlled sleep enabled.

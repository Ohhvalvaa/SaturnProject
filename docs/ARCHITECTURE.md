# Architecture

SaturnProject is one process that does what usually needs three programs (a Remote Play client, a frame
generation tool capturing its window and a launcher), without ever capturing the screen: frames go from the
network decoder to the display entirely on the GPU.

```
PS5 ──Remote Play (chiaki-ng lib)──> H.265/H.264 packets
   └─> FFmpeg D3D11VA decode (GPU)
        └─> duplicate detection (30 fps games: GPU comparison, decode thread)
             └─> NV12 -> BGRA (D3D11 Video Processor)
                  └─> NVIDIA Optical Flow (motion vectors)
                       └─> frame generation: DLSS Frame Generation or FSR 3 (D3D12, shared surfaces + shared fence)
                            └─> frame pacer (timeline cleaned of network jitter, one image per refresh or VRR)
                                 └─> picture tuning (sharpness / shadows / saturation compute shader)
                                      └─> D3D11 Video Processor: scaling + RTX Video Super Resolution
                                           └─> Direct2D UI (cached) -> flip swapchain
Audio: Opus -> WASAPI shared, delayed by the frame generation latency.
Pad:   SDL3 (DualSense through HIDAPI) -> remapping/sticks -> chiaki controller state; rumble, haptics as rumble,
       adaptive triggers and light bar back from the PS5.
```

## Source layout

| File | Role |
|---|---|
| `src/app/main.cpp` | Window, session lifecycle, wake-up/rest mode, 120 Hz switch, log |
| `src/app/video.cpp` | Decode, duplicate detection, engine selection, frame pacer, presentation, screenshots |
| `src/app/dlssg.cpp` | D3D12 bridge: DLSS Frame Generation (with a 2-stage cascade for 30 -> 120) and FSR 3 |
| `src/app/ofi.cpp` | NVIDIA Optical Flow: motion vectors for DLSS/FSR and the fallback interpolation shader |
| `src/app/framegen.cpp` | Resource extraction + optional NVIDIA FRUC engine |
| `src/app/ui.cpp` | Direct2D UI: Home, pairing, tabbed settings panel, in-game menu, statistics, DualSense remapping |
| `src/app/pad.cpp` | SDL3 controller, remapping profiles, dead zone/curves, haptics |
| `src/app/audio.cpp` | Opus -> WASAPI |
| `src/app/console.cpp` | Discovery, state, wake-up, pairing, PSN Account ID sign-in |
| `src/app/settings.cpp` | `%LOCALAPPDATA%\SaturnProject\settings.ini` |
| `src/app/steam.cpp` | When started from Steam, relaunch outside Steam's process tree (real DualSense, no overlay) |
| `src/common/console_store.c` | Paired console stored encrypted with DPAPI |
| `src/cli/main.c` | `saturn-cli` command line tool |

## Key decisions

- **Frame generation on video.** DLSS Frame Generation and FSR 3 are fed like a game would: the real image, motion
  vectors (current -> previous, from NVIDIA Optical Flow at 2x2 grid), a constant depth and a static virtual camera.
  On AMD/Intel GPUs FSR 3 runs with zero vectors and its own internal optical flow.
- **30 fps games.** The PS5 always streams 60 fps; a 30 fps game sends every image twice. Duplicates are detected by
  comparing each frame with the previous one on a 480x272 copy in the decode thread (only for light packets in a
  light/heavy pattern, with hysteresis), then dropped. DLSS runs twice in cascade (midpoint, then 1/4 and 3/4) to
  turn 30 real frames into 120.
- **Frame pacer.** Every image gets its display time on a timeline cleaned of network jitter; with a fixed refresh
  rate one image is shown per refresh, with variable refresh (G-SYNC/FreeSync) each image is presented at its own time.
- **Single executable.** Vendor DLLs are embedded as resources and extracted to `%LOCALAPPDATA%` on first use.

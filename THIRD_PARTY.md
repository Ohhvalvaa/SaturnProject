# Third-party components

| Component | Used for | License |
|---|---|---|
| [chiaki-ng](https://github.com/streetpea/chiaki-ng) v1.10.1 (`lib/`; controller logic adapted from `gui/src/controllermanager.cpp`, PSN sign-in method from `gui/src/psnaccountid.cpp`) | Remote Play protocol, pairing, wake-up, controller | AGPL-3.0-only with OpenSSL exception |
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil, libswresample) | H.264/H.265 decoding with D3D11VA | LGPL-2.1-or-later |
| [SDL3](https://libsdl.org) | Controllers (DualSense through HIDAPI) | zlib |
| [Opus](https://opus-codec.org) | PS5 audio | BSD-3-Clause |
| [OpenSSL](https://www.openssl.org) | Session cryptography | Apache-2.0 |
| json-c, libevent, miniupnpc, nanopb, jerasure / gf-complete, curl | chiaki-ng dependencies | MIT / BSD / zlib / BSD-3-Clause / curl |
| NVIDIA Optical Flow SDK headers (`third_party/nvof`) | Hardware optical flow (the `nvofapi64.dll` runtime ships with the NVIDIA driver) | MIT (headers) |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (NGX headers, `nvsdk_ngx_s.lib`, `nvngx_dlssg.dll`) | DLSS Frame Generation | NVIDIA RTX SDKs License. **Not stored in this repository**: downloaded by `tools/fetch_sdks.ps1` |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (FFX API headers, signed `amd_fidelityfx_*_dx12.dll`) | FSR 3 Frame Generation | AMD FidelityFX SDK license (binary redistribution with notices). **Not stored in this repository**: downloaded by `tools/fetch_sdks.ps1` |
| NVIDIA Optical Flow SDK FRUC (`NvOFFRUC.dll`, CUDA runtime) | Optional fallback engine, only with `SATURN_FRUC=ON` | NVIDIA SDK License / CUDA EULA. Not stored in this repository |

The release build embeds the vendor DLLs listed above inside `SaturnProject.exe` and extracts them to
`%LOCALAPPDATA%\SaturnProject\nvidia` at run time; their own copyright notices and licenses apply to those files.

The SaturnProject logo and user interface are original work (`branding/logo.py`). Segoe UI fonts come with Windows
and are not distributed.

"PlayStation", "PS5", "DualSense" and the PlayStation button symbols are trademarks of Sony Interactive
Entertainment Inc. "NVIDIA", "DLSS", "RTX" and "G-SYNC" are trademarks of NVIDIA Corporation. "AMD", "FSR" and
"FreeSync" are trademarks of Advanced Micro Devices, Inc. SaturnProject is an independent project and is not
affiliated with, endorsed or sponsored by Sony, NVIDIA or AMD.

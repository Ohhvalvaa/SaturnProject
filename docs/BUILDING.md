# Building SaturnProject

SaturnProject is a single Windows executable built with CMake + Ninja, clang-cl and vcpkg (static triplet).

## Prerequisites

| Tool | Tested version | Notes |
|---|---|---|
| Visual Studio 2022 Build Tools | MSVC 14.44, Windows SDK 10.0.26100 | "Desktop development with C++" workload |
| LLVM (clang-cl) | 23.1 | `winget install LLVM.LLVM` |
| CMake | 4.x | `winget install Kitware.CMake` |
| Ninja | 1.13 | `winget install Ninja-build.Ninja` |
| vcpkg | any recent | `git clone https://github.com/microsoft/vcpkg` + `bootstrap-vcpkg.bat` |
| protoc | 36.x | [protobuf releases](https://github.com/protocolbuffers/protobuf/releases), used by chiaki-ng (nanopb) |
| Python 3 | 3.13 | with `pip install protobuf grpcio-tools` (nanopb generator) |

## Steps

```powershell
git clone --recurse-submodules https://github.com/Ohhvalvaa/SaturnProject.git
cd SaturnProject

# NVIDIA DLSS SDK + AMD FidelityFX SDK (frame generation) from their official GitHub repositories
powershell -ExecutionPolicy Bypass -File tools\fetch_sdks.ps1
```

Then, from a "x64 Native Tools Command Prompt for VS 2022":

```bat
set CC=clang-cl
set CXX=clang-cl
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DPROTOC=<protoc>\bin\protoc.exe -DPython3_EXECUTABLE=<python with protobuf>\python.exe
cmake --build build --target SaturnProject saturn-cli
```

vcpkg installs the dependencies listed in `vcpkg.json` (FFmpeg, SDL3, OpenSSL, Opus, json-c, libevent, miniupnpc)
on the first configure; this takes a while.

## Options

| CMake option | Default | Meaning |
|---|---|---|
| `SATURN_FRAMEGEN` | ON if the SDKs are present | DLSS + FSR 3 frame generation (needs `tools/fetch_sdks.ps1`). Without it only the optical flow engine is available |
| `SATURN_FRUC` | OFF | Optional NVIDIA FRUC fallback engine; put `NvOFFRUC.h`, `NvOFFRUC.dll` and `cudart64_110.dll` from the NVIDIA Optical Flow SDK into `third_party/nvofruc` |

## Developer tools

- `tools/dlssg_probe.cpp`: checks DLSS Frame Generation on synthetic images (vector sign and timing).
- `tools/dlssg_bench.cpp`: runs `src/app/dlssg.cpp` (DLSS or FSR 3) without a PS5.
- Diagnostics environment variables: `SATURN_REC`, `SATURN_REC_BACK`, `SATURN_DEBUG_DUPS`, `SATURN_DEBUG_K`,
  `SATURN_OF_EVAL` (see the comments in `src/app/video.cpp`).
- `saturn-cli register|status|wake|play|standby`: command line pairing, wake-up and session test.

cmake_minimum_required(VERSION 3.24)
project(SaturnProject VERSION 1.0.0 LANGUAGES C CXX)

# --- chiaki-ng: only the Remote Play library (no GUI/CLI/tests) ---
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_BOREALIS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
set(CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER OFF CACHE BOOL "" FORCE)
set(CHIAKI_USE_SYSTEM_JERASURE OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_NANOPB OFF CACHE STRING "" FORCE)
add_subdirectory(third_party/chiaki-ng EXCLUDE_FROM_ALL)

# --- saturn-cli: command line tool (wake, session test, rest mode) ---
add_executable(saturn-cli src/cli/main.c src/common/console_store.c)
target_include_directories(saturn-cli PRIVATE src/common)
target_link_libraries(saturn-cli PRIVATE chiaki-lib crypt32)
target_compile_definitions(saturn-cli PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)

# --- SaturnProject: the app (window, D3D11VA video, WASAPI audio, SDL3 pad, frame generation) ---
find_package(FFMPEG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
add_executable(SaturnProject WIN32
	src/app/main.cpp src/app/video.cpp src/app/audio.cpp src/app/pad.cpp src/app/framegen.cpp src/app/ofi.cpp src/app/dlssg.cpp src/app/settings.cpp src/app/console.cpp src/app/ui.cpp src/app/steam.cpp src/app/saturn.rc
	src/common/console_store.c)
set_target_properties(SaturnProject PROPERTIES CXX_STANDARD 17)
target_include_directories(SaturnProject PRIVATE src/common ${FFMPEG_INCLUDE_DIRS})
target_link_directories(SaturnProject PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(SaturnProject PRIVATE chiaki-lib ${FFMPEG_LIBRARIES} SDL3::SDL3-static
	d3d11 d3d12 dxgi d3dcompiler d2d1 dwrite windowscodecs winhttp wbemuuid oleaut32 dwmapi crypt32 winmm ws2_32 ole32 shell32)
target_compile_definitions(SaturnProject PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# The vendor SDKs are not stored in this repository (their licenses are not AGPL-compatible):
# tools/fetch_sdks.ps1 downloads them from the official NVIDIA and AMD GitHub repositories into third_party/.
# Their runtime DLLs are embedded in the exe (saturn.rc) and extracted to %LOCALAPPDATA%/SaturnProject/nvidia
# only when needed: the app stays a single file.
set(SATURN_RC_DEPS ${CMAKE_SOURCE_DIR}/branding/saturn-logo.png)

# DLSS Frame Generation (NVIDIA) + FSR 3 Frame Generation (AMD): on automatically when both SDKs are present.
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/dlss/include/nvsdk_ngx.h AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/fsr/api/include/ffx_api.h)
	set(SATURN_FRAMEGEN_DEFAULT ON)
else()
	set(SATURN_FRAMEGEN_DEFAULT OFF)
endif()
option(SATURN_FRAMEGEN "DLSS + FSR 3 frame generation (run tools/fetch_sdks.ps1 first)" ${SATURN_FRAMEGEN_DEFAULT})
if(SATURN_FRAMEGEN)
	target_link_libraries(SaturnProject PRIVATE ${CMAKE_SOURCE_DIR}/third_party/dlss/lib/nvsdk_ngx_s.lib)
	list(APPEND SATURN_RC_DEPS ${CMAKE_SOURCE_DIR}/third_party/dlss/lib/nvngx_dlssg.dll
		${CMAKE_SOURCE_DIR}/third_party/fsr/bin/amd_fidelityfx_loader_dx12.dll ${CMAKE_SOURCE_DIR}/third_party/fsr/bin/amd_fidelityfx_framegeneration_dx12.dll)
else()
	target_compile_definitions(SaturnProject PRIVATE SATURN_NO_DLSS SATURN_NO_FSR)
	message(STATUS "SaturnProject: building without DLSS/FSR frame generation (optical flow engine only)")
endif()

# Optional NVIDIA FRUC fallback engine: needs NvOFFRUC.h, NvOFFRUC.dll and cudart64_110.dll in third_party/nvofruc
# (NVIDIA Optical Flow SDK). Off by default: DLSS, FSR 3 and the optical flow engine cover every GPU.
option(SATURN_FRUC "Optional NVIDIA FRUC fallback engine" OFF)
if(SATURN_FRUC)
	list(APPEND SATURN_RC_DEPS ${CMAKE_SOURCE_DIR}/third_party/nvofruc/NvOFFRUC.dll ${CMAKE_SOURCE_DIR}/third_party/nvofruc/cudart64_110.dll)
else()
	target_compile_definitions(SaturnProject PRIVATE SATURN_NO_FRUC)
endif()
set_property(SOURCE src/app/saturn.rc APPEND PROPERTY OBJECT_DEPENDS ${SATURN_RC_DEPS})

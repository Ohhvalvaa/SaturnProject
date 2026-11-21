// Steam integration.
#pragma once
#include <windows.h>

// Started from Steam? Then relaunch the app OUTSIDE Steam's process tree (through WMI) and wait for it to finish:
// this way Steam Input does not replace the DualSense with a virtual pad and the overlay is not injected.
// Returns true if this process was only the "launcher" and must exit with *exit_code.
bool steam_relaunch_detached(const wchar_t *cmdline, int *exit_code);
// If the app was relaunched by the Steam launcher: the launcher's pid (0 otherwise). When the launcher dies
// (Steam "Stop") the app closes normally (PS5 to rest mode).
DWORD steam_launcher_pid(const wchar_t *cmdline);

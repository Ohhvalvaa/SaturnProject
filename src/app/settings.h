// App settings: %LOCALAPPDATA%\SaturnProject\settings.ini (plain text, can be edited by hand).
// Every entry has a sensible default: a missing or broken file never blocks start-up.
#pragma once

enum Lang { LANG_AUTO, LANG_IT, LANG_EN };
// AUTO = "Maximum": duplicates dropped, up to the display refresh rate; X2 = "Light": 30 -> 60;
// SMOOTH (FG_SMOOTH) = "Automatic" (default): one or three generated frames per real one depending on the source
enum FgMode { FG_OFF, FG_AUTO, FG_X2, FG_SMOOTH };
// DLSS = NVIDIA DLSS Frame Generation (best quality); NVOF = optical flow + our shader; FRUC = optional fallback
// FSR = AMD FSR 3 Frame Generation (any DirectX 12 GPU)
enum FgEngine { FGE_NVOF, FGE_FRUC, FGE_DLSS, FGE_FSR };

struct Settings
{
	// start-up
	bool quick_start = true;     // connects right away and closes the app when leaving the game
	bool wake = true;            // wakes the PS5 from rest mode
	bool rest_on_exit = true;    // puts the PS5 back in rest mode on exit
	int lang = LANG_AUTO;
	// PS5 stream
	int resolution = 1080;       // 360, 540, 720, 1080, 1440, 2160 (1440/2160 only on PS5 models that support them)
	int fps = 60;                // 30, 60
	int codec = 1;               // 0 H.264, 1 H.265
	int bitrate = 0;             // kbit/s; 0 = automatic from the resolution
	// picture
	int fg = FG_SMOOTH;
	int fg_engine = FGE_DLSS;
	bool vsr = true;             // RTX Video Super Resolution
	bool fullscreen = true;
	bool vrr = false;            // variable refresh (FreeSync/G-Sync active in the driver): every frame at its own time
	bool hz120 = true;           // switches the display to 120 Hz while playing (if supported)
	bool stats = false;          // statistics panel visible when the game starts
	int sharpen = 0;             // sharpness 0-100 (contrast-adaptive filter)
	int shadows = 0;             // shadow lift 0-100
	int saturation = 100;        // saturation 50-150 %
	bool competitive = false;    // competitive mode: no frame generation, minimum latency
	// audio
	int volume = 100;            // 0-100
	int audio_delay = 0;         // extra ms (fine sync adjustment), 0-200
	// pad
	bool rumble = true;
	bool haptics_rumble = true;  // PS5 haptics translated into rumble
	bool adaptive_triggers = true;
	bool motion = false;         // motion sensors: not used (pad motion is not sent to the PS5)
	int deadzone = 6;            // stick dead zone, % (0-30)
	int stick_curve = 0;         // 0 linear, 1 soft (precise near center), 2 fast
	// remapping: 3 named profiles; for each pressed button the one that is sent (order: Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Create)
	int key_profile = 0;
	wchar_t kp_name[3][24] = { L"Profile 1", L"Profile 2", L"Profile 3" };
	int kp_map[3][12] = { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
};

extern Settings g_set;
void settings_load();
void settings_save();
const char *settings_dir(); // %LOCALAPPDATA%\SaturnProject (no trailing slash)
int settings_bitrate();      // effective kbit/s
bool lang_it();              // Italian UI language active
inline const wchar_t *T(const wchar_t *it, const wchar_t *en) { return lang_it() ? it : en; }

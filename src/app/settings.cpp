#include "settings.h"
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

Settings g_set;
static char g_dir[MAX_PATH], g_ini[MAX_PATH];

const char *settings_dir()
{
	if (!g_dir[0])
	{
		char base[MAX_PATH] = ".";
		SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base);
		snprintf(g_dir, sizeof(g_dir), "%s\\SaturnProject", base);
		CreateDirectoryA(g_dir, nullptr);
		snprintf(g_ini, sizeof(g_ini), "%s\\settings.ini", g_dir);
	}
	return g_dir;
}

static int geti(const char *sec, const char *key, int def, int lo, int hi)
{
	int v = (int)GetPrivateProfileIntA(sec, key, def, g_ini);
	return v < lo || v > hi ? def : v;
}
static bool getb(const char *sec, const char *key, bool def) { return geti(sec, key, def ? 1 : 0, 0, 1) != 0; }
static void puti(const char *sec, const char *key, int v)
{
	char b[16]; snprintf(b, sizeof(b), "%d", v);
	WritePrivateProfileStringA(sec, key, b, g_ini);
}

static int snap(int v, const int *allowed, int n, int def)
{
	for (int i = 0; i < n; i++) if (allowed[i] == v) return v;
	return def;
}

void settings_load()
{
	settings_dir();
	Settings d;
	Settings &s = g_set;
	s.quick_start = getb("startup", "quick_start", d.quick_start);
	s.wake = getb("startup", "wake_ps5", d.wake);
	s.rest_on_exit = getb("startup", "rest_on_exit", d.rest_on_exit);
	s.lang = geti("startup", "language", d.lang, 0, 2);
	static const int res[] = { 360, 540, 720, 1080, 1440, 2160 };
	s.resolution = snap(geti("stream", "resolution", d.resolution, 0, 9999), res, 6, d.resolution);
	static const int fps[] = { 30, 60 };
	s.fps = snap(geti("stream", "fps", d.fps, 0, 999), fps, 2, d.fps);
	s.codec = geti("stream", "codec_h265", d.codec, 0, 1);
	s.bitrate = geti("stream", "bitrate_kbit", d.bitrate, 0, 200000);
	s.fg = geti("picture", "frame_generation", d.fg, 0, 3);
	s.fg_engine = geti("picture", "frame_generation_engine", d.fg_engine, 0, 3);
	s.vsr = getb("picture", "rtx_vsr", d.vsr);
	s.fullscreen = getb("picture", "fullscreen", d.fullscreen);
	s.hz120 = getb("picture", "display_120hz", d.hz120);
	s.vrr = getb("picture", "variable_refresh", d.vrr);
	s.stats = getb("picture", "statistics", d.stats);
	s.volume = geti("audio", "volume", d.volume, 0, 100);
	s.audio_delay = geti("audio", "delay_ms", d.audio_delay, 0, 200);
	s.rumble = getb("pad", "rumble", d.rumble);
	s.haptics_rumble = getb("pad", "haptics_as_rumble", d.haptics_rumble);
	s.adaptive_triggers = getb("pad", "adaptive_triggers", d.adaptive_triggers);
	s.motion = getb("pad", "motion", d.motion);
	s.sharpen = geti("picture", "sharpness", d.sharpen, 0, 100);
	s.shadows = geti("picture", "shadows", d.shadows, 0, 100);
	s.saturation = geti("picture", "saturation", d.saturation, 50, 150);
	s.competitive = getb("picture", "competitive", d.competitive);
	s.deadzone = geti("pad", "dead_zone", d.deadzone, 0, 30);
	s.stick_curve = geti("pad", "stick_curve", d.stick_curve, 0, 2);
	s.key_profile = geti("buttons", "profile", d.key_profile, 0, 2);
	for (int p = 0; p < 3; p++)
	{
		char k[16]; snprintf(k, 16, "name%d", p);
		wchar_t wk[16]; MultiByteToWideChar(CP_ACP, 0, k, -1, wk, 16);
		wchar_t wini[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, g_ini, -1, wini, MAX_PATH);
		GetPrivateProfileStringW(L"buttons", wk, d.kp_name[p], s.kp_name[p], 24, wini);
		if (!s.kp_name[p][0]) wcscpy(s.kp_name[p], d.kp_name[p]);
		for (int i = 0; i < 12; i++) { snprintf(k, 16, "p%d_t%d", p, i); s.kp_map[p][i] = geti("buttons", k, d.kp_map[p][i], 0, 11); }
	}
	if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) settings_save(); // first start: file with the default values
}

void settings_save()
{
	settings_dir();
	const Settings &s = g_set;
	puti("startup", "quick_start", s.quick_start);
	puti("startup", "wake_ps5", s.wake);
	puti("startup", "rest_on_exit", s.rest_on_exit);
	puti("startup", "language", s.lang);
	puti("stream", "resolution", s.resolution);
	puti("stream", "fps", s.fps);
	puti("stream", "codec_h265", s.codec);
	puti("stream", "bitrate_kbit", s.bitrate);
	puti("picture", "frame_generation", s.fg);
	puti("picture", "frame_generation_engine", s.fg_engine);
	puti("picture", "rtx_vsr", s.vsr);
	puti("picture", "fullscreen", s.fullscreen);
	puti("picture", "display_120hz", s.hz120);
	puti("picture", "variable_refresh", s.vrr);
	puti("picture", "statistics", s.stats);
	puti("audio", "volume", s.volume);
	puti("audio", "delay_ms", s.audio_delay);
	puti("pad", "rumble", s.rumble);
	puti("pad", "haptics_as_rumble", s.haptics_rumble);
	puti("pad", "adaptive_triggers", s.adaptive_triggers);
	puti("pad", "motion", s.motion);
	puti("picture", "sharpness", s.sharpen);
	puti("picture", "shadows", s.shadows);
	puti("picture", "saturation", s.saturation);
	puti("picture", "competitive", s.competitive);
	puti("pad", "dead_zone", s.deadzone);
	puti("pad", "stick_curve", s.stick_curve);
	puti("buttons", "profile", s.key_profile);
	for (int p = 0; p < 3; p++)
	{
		char k[16]; snprintf(k, 16, "name%d", p);
		wchar_t wk[16]; MultiByteToWideChar(CP_ACP, 0, k, -1, wk, 16);
		wchar_t wini[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, g_ini, -1, wini, MAX_PATH);
		WritePrivateProfileStringW(L"buttons", wk, s.kp_name[p], wini);
		for (int i = 0; i < 12; i++) { snprintf(k, 16, "p%d_t%d", p, i); puti("buttons", k, s.kp_map[p][i]); }
	}
}

int settings_bitrate()
{
	if (g_set.bitrate > 0) return g_set.bitrate;
	// automatic: like the PS5 / chiaki-ng, a little higher (local network)
	switch (g_set.resolution)
	{
	case 360: return 2000;
	case 540: return 6000;
	case 720: return 10000;
	case 1080: return g_set.codec ? 50000 : 30000;
	case 1440: return 60000;
	default: return 80000;
	}
}

bool lang_it()
{
	if (g_set.lang == LANG_IT) return true;
	if (g_set.lang == LANG_EN) return false;
	static int sys = -1;
	if (sys < 0) sys = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN ? 1 : 0;
	return sys == 1;
}

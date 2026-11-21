// Pad: SDL3 reads the controller (DualSense through HIDAPI: buttons, sticks, analog triggers, touchpad)
// and sends it to the PS5 as soon as something changes. From the PS5 come back rumble, adaptive triggers
// and light bar. Logic adapted from chiaki-ng (gui/src/controllermanager.cpp, AGPL).
#include <algorithm>
#include <math.h>
#include "app.h"
#include "settings.h"
#include "ui.h"
#include <SDL3/SDL.h>
#include <chiaki/controller.h>
#include <chiaki/orientation.h>
#include <math.h>
#include <stdio.h>

#define PS_TOUCHPAD_MAXX 1920
#define PS_TOUCHPAD_MAXY 1079

// DualSense effects report (from SDL, zlib license)
typedef struct
{
	Uint8 ucEnableBits1, ucEnableBits2, ucRumbleRight, ucRumbleLeft;
	Uint8 ucHeadphoneVolume, ucSpeakerVolume, ucMicrophoneVolume, ucAudioEnableBits, ucMicLightMode, ucAudioMuteBits;
	Uint8 rgucRightTriggerEffect[11], rgucLeftTriggerEffect[11];
	Uint8 rgucUnknown1[6];
	Uint8 ucEnableBits3;
	Uint8 rgucUnknown2[2];
	Uint8 ucLedAnim, ucLedBrightness, ucPadLights, ucLedRed, ucLedGreen, ucLedBlue;
} DS5EffectsState_t;

static ChiakiSession *volatile g_session;
static char g_name[128];
static HANDLE g_thread;
static volatile bool g_run;
static SDL_Gamepad *g_pad;
static bool g_ds, g_edge;
static uint16_t g_fw;
static uint8_t g_intensity = 0x00; // triggers | rumble, like chiaki-ng
static float g_rumble_mult = 1.0f;
static bool g_haptic_active;

static ChiakiControllerState g_state, g_sent;
static ChiakiOrientationTracker g_orient;
static ChiakiAccelNewZero g_accel_zero, g_real_accel;
static int8_t g_touch_ids[2][4]; // [touchpad][finger] -> chiaki id
static SRWLOCK g_pad_lock = SRWLOCK_INIT;

static void reset_motion()
{
	chiaki_accel_new_zero_set_active(&g_accel_zero, g_real_accel.accel_x, g_real_accel.accel_y, g_real_accel.accel_z, false);
	chiaki_orientation_tracker_init(&g_orient);
}

static void send_effect(DS5EffectsState_t *s)
{
	AcquireSRWLockShared(&g_pad_lock);
	if (g_pad) SDL_SendGamepadEffect(g_pad, s, sizeof(*s));
	ReleaseSRWLockShared(&g_pad_lock);
}

static void ds_rumble(uint8_t left, uint8_t right)
{
	DS5EffectsState_t s = {};
	if (!g_edge && g_fw != 0 && g_fw < 0x0224) { s.ucEnableBits1 |= 0x01; s.ucRumbleLeft = left >> 1; s.ucRumbleRight = right >> 1; }
	else { s.ucEnableBits3 |= 0x04; s.ucRumbleLeft = left; s.ucRumbleRight = right; }
	s.rgucUnknown1[4] = g_intensity;
	s.ucEnableBits2 |= 0x40;
	s.ucEnableBits1 |= 0x02;
	send_effect(&s);
}

static void rumble(uint8_t left, uint8_t right)
{
	if (!g_set.rumble && (left || right)) return;
	if (g_ds) { ds_rumble(left, right); return; }
	AcquireSRWLockShared(&g_pad_lock);
	if (g_pad) SDL_RumbleGamepad(g_pad, (Uint16)(left << 8), (Uint16)(right << 8), 5000);
	ReleaseSRWLockShared(&g_pad_lock);
}

static void trigger_effects(uint8_t tl, const uint8_t *dl, uint8_t tr, const uint8_t *dr)
{
	if (!g_ds) return;
	static const uint8_t off[10] = { 0 };
	if (!g_set.adaptive_triggers) { tl = tr = 0x05; dl = dr = off; }
	DS5EffectsState_t s = {};
	s.rgucUnknown1[4] = g_intensity;
	s.ucEnableBits2 |= 0x40;
	s.ucEnableBits1 |= 0x04 | 0x08;
	s.rgucLeftTriggerEffect[0] = tl; memcpy(s.rgucLeftTriggerEffect + 1, dl, 10);
	s.rgucRightTriggerEffect[0] = tr; memcpy(s.rgucRightTriggerEffect + 1, dr, 10);
	send_effect(&s);
}

static void open_pad(SDL_JoystickID id)
{
	if (g_pad) return;
	SDL_Gamepad *p = SDL_OpenGamepad(id);
	if (!p) return;
	AcquireSRWLockExclusive(&g_pad_lock);
	g_pad = p;
	SDL_GamepadType t = SDL_GetGamepadType(p);
	g_ds = t == SDL_GAMEPAD_TYPE_PS5;
	g_edge = g_ds && SDL_GetGamepadProduct(p) == 0x0DF2;
	g_fw = SDL_GetGamepadFirmwareVersion(p);
	ReleaseSRWLockExclusive(&g_pad_lock);
	SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_GYRO, true);
	SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_ACCEL, true);
	reset_motion();
	snprintf(g_name, sizeof(g_name), "%s", SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "Pad");
	fprintf(stderr, "Pad: %s%s\n", g_name, g_ds ? " (DualSense)" : "");
}

static void close_pad()
{
	if (!g_pad) return;
	if (g_ds) { uint8_t off[10] = { 0 }; trigger_effects(0x05, off, 0x05, off); }
	rumble(0, 0);
	AcquireSRWLockExclusive(&g_pad_lock);
	SDL_CloseGamepad(g_pad);
	g_pad = nullptr;
	g_name[0] = 0;
	ReleaseSRWLockExclusive(&g_pad_lock);
	chiaki_controller_state_set_idle(&g_state);
}

static uint32_t button_bit(Uint8 b)
{
	switch (b)
	{
	case SDL_GAMEPAD_BUTTON_SOUTH: return CHIAKI_CONTROLLER_BUTTON_CROSS;
	case SDL_GAMEPAD_BUTTON_EAST: return CHIAKI_CONTROLLER_BUTTON_MOON;
	case SDL_GAMEPAD_BUTTON_WEST: return CHIAKI_CONTROLLER_BUTTON_BOX;
	case SDL_GAMEPAD_BUTTON_NORTH: return CHIAKI_CONTROLLER_BUTTON_PYRAMID;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return CHIAKI_CONTROLLER_BUTTON_L1;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return CHIAKI_CONTROLLER_BUTTON_R1;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return CHIAKI_CONTROLLER_BUTTON_L3;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return CHIAKI_CONTROLLER_BUTTON_R3;
	case SDL_GAMEPAD_BUTTON_START: return CHIAKI_CONTROLLER_BUTTON_OPTIONS;
	case SDL_GAMEPAD_BUTTON_BACK: return CHIAKI_CONTROLLER_BUTTON_SHARE;
	case SDL_GAMEPAD_BUTTON_TOUCHPAD: return CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	case SDL_GAMEPAD_BUTTON_GUIDE: return CHIAKI_CONTROLLER_BUTTON_PS;
	default: return 0;
	}
}

// ---------- the pad in the user interface ----------
// The DualSense Mute button (or Create+Options together) opens/closes the menu: it never reaches the PS5.
// While the UI has control (Home, settings, open menu) buttons go to it and the PS5 receives
// the pad at rest. D-pad and left stick repeat while held.
static int16_t g_nav_lx, g_nav_ly;
static int g_nav_dir = -1;        // UiPad of the held direction
static DWORD g_nav_next;
static bool g_btn_down[SDL_GAMEPAD_BUTTON_COUNT];

static void nav_press(UiPad b) { ui_pad(b); }

static bool ui_nav_button(Uint8 b, bool down)
{
	if (b < SDL_GAMEPAD_BUTTON_COUNT) g_btn_down[b] = down;
	if (ui_remap_listening() && b != SDL_GAMEPAD_BUTTON_MISC1)
	{
		// remapping: the pressed button is the one to send
		int idx = -1;
		switch (b)
		{
		case SDL_GAMEPAD_BUTTON_SOUTH: idx = 0; break; case SDL_GAMEPAD_BUTTON_EAST: idx = 1; break;
		case SDL_GAMEPAD_BUTTON_WEST: idx = 2; break; case SDL_GAMEPAD_BUTTON_NORTH: idx = 3; break;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: idx = 4; break; case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: idx = 5; break;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: idx = 8; break; case SDL_GAMEPAD_BUTTON_RIGHT_STICK: idx = 9; break;
		case SDL_GAMEPAD_BUTTON_START: idx = 10; break; case SDL_GAMEPAD_BUTTON_BACK: idx = 11; break;
		default: break;
		}
		if (down && idx >= 0) ui_remap_pressed(idx);
		return true;
	}
	if (b == SDL_GAMEPAD_BUTTON_MISC1) { if (down) nav_press(UP_MENU); return true; }
	if (down && ((b == SDL_GAMEPAD_BUTTON_START && g_btn_down[SDL_GAMEPAD_BUTTON_BACK]) || (b == SDL_GAMEPAD_BUTTON_BACK && g_btn_down[SDL_GAMEPAD_BUTTON_START])))
	{
		nav_press(UP_MENU);
		return true;
	}
	if (!ui_captures_pad()) return false;
	int dir = -1;
	switch (b)
	{
	case SDL_GAMEPAD_BUTTON_DPAD_UP: dir = UP_UP; break;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dir = UP_DOWN; break;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dir = UP_LEFT; break;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dir = UP_RIGHT; break;
	}
	if (dir >= 0)
	{
		if (down) { nav_press((UiPad)dir); g_nav_dir = dir; g_nav_next = GetTickCount() + 380; }
		else if (g_nav_dir == dir) g_nav_dir = -1;
		return true;
	}
	if (!down) return true;
	switch (b)
	{
	case SDL_GAMEPAD_BUTTON_SOUTH: nav_press(UP_OK); break;
	case SDL_GAMEPAD_BUTTON_EAST: nav_press(UP_BACK); break;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: nav_press(UP_L1); break;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: nav_press(UP_R1); break;
	case SDL_GAMEPAD_BUTTON_START: nav_press(UP_MENU); break;
	default: break;
	}
	return true;
}

// stick as d-pad + repeat; called on every loop of the thread
static void nav_tick()
{
	if (!ui_captures_pad()) { g_nav_dir = -1; return; }
	static int stick_dir = -1;
	int sd = -1;
	if (g_nav_ly < -20000) sd = UP_UP; else if (g_nav_ly > 20000) sd = UP_DOWN;
	else if (g_nav_lx < -20000) sd = UP_LEFT; else if (g_nav_lx > 20000) sd = UP_RIGHT;
	if (sd != stick_dir)
	{
		stick_dir = sd;
		if (sd >= 0) { nav_press((UiPad)sd); g_nav_dir = sd; g_nav_next = GetTickCount() + 380; }
		else if (g_nav_dir >= 0 && !g_btn_down[SDL_GAMEPAD_BUTTON_DPAD_UP] && !g_btn_down[SDL_GAMEPAD_BUTTON_DPAD_DOWN]
			&& !g_btn_down[SDL_GAMEPAD_BUTTON_DPAD_LEFT] && !g_btn_down[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) g_nav_dir = -1;
	}
	if (g_nav_dir >= 0 && (int)(GetTickCount() - g_nav_next) >= 0) { nav_press((UiPad)g_nav_dir); g_nav_next = GetTickCount() + 110; }
}

static void handle(const SDL_Event &e)
{
	switch (e.type)
	{
	case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
	case SDL_EVENT_GAMEPAD_REMOVED:
		if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) close_pad();
		break;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
	{
		if (ui_nav_button(e.gbutton.button, e.gbutton.down)) break;
		uint32_t bit = button_bit(e.gbutton.button);
		if (e.gbutton.down) g_state.buttons |= bit; else g_state.buttons &= ~bit;
		break;
	}
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		switch (e.gaxis.axis)
		{
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: g_state.l2_state = (uint8_t)(e.gaxis.value >> 7);  if (e.gaxis.value > 16000 && ui_remap_listening()) ui_remap_pressed(6); break;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: g_state.r2_state = (uint8_t)(e.gaxis.value >> 7);  if (e.gaxis.value > 16000 && ui_remap_listening()) ui_remap_pressed(7); break;
		case SDL_GAMEPAD_AXIS_LEFTX: g_state.left_x = e.gaxis.value; g_nav_lx = e.gaxis.value; break;
		case SDL_GAMEPAD_AXIS_LEFTY: g_state.left_y = e.gaxis.value; g_nav_ly = e.gaxis.value; break;
		case SDL_GAMEPAD_AXIS_RIGHTX: g_state.right_x = e.gaxis.value; break;
		case SDL_GAMEPAD_AXIS_RIGHTY: g_state.right_y = e.gaxis.value; break;
		}
		break;
	case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
	case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
	case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
	{
		int tp = e.gtouchpad.touchpad & 1, fi = e.gtouchpad.finger & 3;
		uint16_t x = (uint16_t)(e.gtouchpad.x * PS_TOUCHPAD_MAXX), y = (uint16_t)(e.gtouchpad.y * PS_TOUCHPAD_MAXY);
		if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) g_touch_ids[tp][fi] = chiaki_controller_state_start_touch(&g_state, x, y);
		else if (g_touch_ids[tp][fi] >= 0)
		{
			if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION) chiaki_controller_state_set_touch_pos(&g_state, (uint8_t)g_touch_ids[tp][fi], x, y);
			else { chiaki_controller_state_stop_touch(&g_state, (uint8_t)g_touch_ids[tp][fi]); g_touch_ids[tp][fi] = -1; }
		}
		break;
	}
	case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
	{
		uint32_t ts = (uint32_t)(e.gsensor.sensor_timestamp / 1000); // ns -> us
		const float *d = e.gsensor.data;
		if (e.gsensor.sensor == SDL_SENSOR_ACCEL)
		{
			float ax = d[0] / SDL_STANDARD_GRAVITY, ay = d[1] / SDL_STANDARD_GRAVITY, az = d[2] / SDL_STANDARD_GRAVITY;
			chiaki_accel_new_zero_set_active(&g_real_accel, ax, ay, az, true);
			chiaki_orientation_tracker_update(&g_orient, g_state.gyro_x, g_state.gyro_y, g_state.gyro_z, ax, ay, az, &g_accel_zero, false, ts);
		}
		else if (e.gsensor.sensor == SDL_SENSOR_GYRO)
			chiaki_orientation_tracker_update(&g_orient, d[0], d[1], d[2], g_state.accel_x, g_state.accel_y, g_state.accel_z, &g_accel_zero, true, ts);
		else break;
			// pad motion is not sent to the PS5
		break;
	}
	default: break;
	}
}

// ---------- profile: remapping and sticks ----------
int pad_battery(bool *charging)
{
	int pct = -1;
	AcquireSRWLockShared(&g_pad_lock);
	SDL_PowerState st = g_pad ? SDL_GetGamepadPowerInfo(g_pad, &pct) : SDL_POWERSTATE_UNKNOWN;
	ReleaseSRWLockShared(&g_pad_lock);
	if (charging) *charging = st == SDL_POWERSTATE_CHARGING;
	return st == SDL_POWERSTATE_ON_BATTERY || st == SDL_POWERSTATE_CHARGING ? pct : -1;
}

static int16_t clamp16(float v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// radial dead zone and response curve
static void shape_stick(int16_t &x, int16_t &y)
{
	float fx = x / 32767.f, fy = y / 32767.f, r = sqrtf(fx * fx + fy * fy);
	float dz = g_set.deadzone / 100.f;
	if (r <= dz || r < 1e-4f) { x = y = 0; return; }
	float t = (std::min(r, 1.f) - dz) / (1 - dz);
	if (g_set.stick_curve == 1) t = 0.4f * t + 0.6f * t * t; // soft: more precise near the center
	else if (g_set.stick_curve == 2) t = sqrtf(t);                                                     // fast
	float k = t / r;
	x = clamp16(fx * k * 32767); y = clamp16(fy * k * 32767);
}

// remapping: for each pressed button (order Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Create) the one to send
static void remap_apply(ChiakiControllerState *o)
{
	static const uint32_t RB[12] = { CHIAKI_CONTROLLER_BUTTON_CROSS, CHIAKI_CONTROLLER_BUTTON_MOON, CHIAKI_CONTROLLER_BUTTON_BOX,
		CHIAKI_CONTROLLER_BUTTON_PYRAMID, CHIAKI_CONTROLLER_BUTTON_L1, CHIAKI_CONTROLLER_BUTTON_R1, 0, 0,
		CHIAKI_CONTROLLER_BUTTON_L3, CHIAKI_CONTROLLER_BUTTON_R3, CHIAKI_CONTROLLER_BUTTON_OPTIONS, CHIAKI_CONTROLLER_BUTTON_SHARE };
	bool ident = true; for (int i = 0; i < 12; i++) if (g_set.kp_map[g_set.key_profile][i] != i) ident = false;
	if (ident) return;
	bool src[12]; uint8_t l2a = o->l2_state, r2a = o->r2_state;
	for (int i = 0; i < 12; i++) src[i] = RB[i] ? (o->buttons & RB[i]) != 0 : (i == 6 ? l2a : r2a) > 64;
	for (int i = 0; i < 12; i++) if (RB[i]) o->buttons &= ~RB[i];
	o->l2_state = o->r2_state = 0;
	for (int i = 0; i < 12; i++)
	{
		int t = g_set.kp_map[g_set.key_profile][i];
		if (t < 0 || t > 11) t = i;
		if (t == 6 && i == 6) { o->l2_state = std::max(o->l2_state, l2a); continue; } // analog kept
		if (t == 7 && i == 7) { o->r2_state = std::max(o->r2_state, r2a); continue; }
		if (!src[i]) continue;
		if (RB[t]) o->buttons |= RB[t]; else if (t == 6) o->l2_state = 255; else o->r2_state = 255;
	}
}

static void profile_apply(ChiakiControllerState *o)
{
	shape_stick(o->left_x, o->left_y);
	shape_stick(o->right_x, o->right_y);
	remap_apply(o);
}

// ---------- test commands (for tests only, without a pad in hand) ----------
// Write %LOCALAPPDATA%\SaturnProject\input.txt; the app reads it, deletes it and runs it.
// Words separated by spaces: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3
// options share ps touchpad (short press), wNNN (wait ms), hold:<button>:<ms>,
// ls:<x>:<y>:<ms> / rs:<x>:<y>:<ms> (stick, values -32768..32767).
struct Step { uint32_t buttons; uint8_t l2, r2; int16_t lx, ly, rx, ry; bool stick; DWORD ms; };
static Step g_steps[256];
static int g_nsteps, g_cur = -1;
static DWORD g_step_end;

static uint32_t name_bit(const char *n, uint8_t *l2, uint8_t *r2)
{
	static const struct { const char *n; uint32_t b; } t[] = {
		{ "cross", CHIAKI_CONTROLLER_BUTTON_CROSS }, { "circle", CHIAKI_CONTROLLER_BUTTON_MOON },
		{ "square", CHIAKI_CONTROLLER_BUTTON_BOX }, { "triangle", CHIAKI_CONTROLLER_BUTTON_PYRAMID },
		{ "up", CHIAKI_CONTROLLER_BUTTON_DPAD_UP }, { "down", CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
		{ "left", CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT }, { "right", CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
		{ "l1", CHIAKI_CONTROLLER_BUTTON_L1 }, { "r1", CHIAKI_CONTROLLER_BUTTON_R1 },
		{ "l3", CHIAKI_CONTROLLER_BUTTON_L3 }, { "r3", CHIAKI_CONTROLLER_BUTTON_R3 },
		{ "options", CHIAKI_CONTROLLER_BUTTON_OPTIONS }, { "share", CHIAKI_CONTROLLER_BUTTON_SHARE },
		{ "ps", CHIAKI_CONTROLLER_BUTTON_PS }, { "touchpad", CHIAKI_CONTROLLER_BUTTON_TOUCHPAD } };
	if (!strcmp(n, "l2")) { *l2 = 255; return 0; }
	if (!strcmp(n, "r2")) { *r2 = 255; return 0; }
	for (auto &e : t) if (!strcmp(n, e.n)) return e.b;
	return 0;
}

static void script_load()
{
	static DWORD last;
	if (GetTickCount() - last < 100) return;
	last = GetTickCount();
	char path[MAX_PATH];
	if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH)) return;
	strcat(path, "\\SaturnProject\\input.txt");
	FILE *f = fopen(path, "rb");
	if (!f) return;
	char buf[4096] = { 0 };
	fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	DeleteFileA(path);
	g_nsteps = 0; g_cur = -1;
	for (char *tok = strtok(buf, " \r\n\t"); tok && g_nsteps < 254; tok = strtok(nullptr, " \r\n\t"))
	{
		Step s = {};
		if (tok[0] == 'w' && tok[1] >= '0' && tok[1] <= '9') { s.ms = (DWORD)atoi(tok + 1); g_steps[g_nsteps++] = s; continue; }
		if (!strncmp(tok, "ls:", 3) || !strncmp(tok, "rs:", 3))
		{
			int x = 0, y = 0, ms = 0;
			sscanf(tok + 3, "%d:%d:%d", &x, &y, &ms);
			s.stick = true; s.ms = (DWORD)ms;
			if (tok[0] == 'l') { s.lx = (int16_t)x; s.ly = (int16_t)y; } else { s.rx = (int16_t)x; s.ry = (int16_t)y; }
			g_steps[g_nsteps++] = s;
			continue;
		}
		if (!strncmp(tok, "hold:", 5))
		{
			char n[32] = { 0 }; int ms = 0;
			char *c = strchr(tok + 5, ':');
			if (!c) continue;
			memcpy(n, tok + 5, (size_t)(c - tok - 5) < 31 ? (size_t)(c - tok - 5) : 31);
			ms = atoi(c + 1);
			s.buttons = name_bit(n, &s.l2, &s.r2); s.ms = (DWORD)ms;
			g_steps[g_nsteps++] = s;
		}
		else
		{
			s.buttons = name_bit(tok, &s.l2, &s.r2); s.ms = 120;
			g_steps[g_nsteps++] = s;
		}
		Step rel = {}; rel.ms = 150; // release between one press and the next
		g_steps[g_nsteps++] = rel;
	}
	fprintf(stderr, "Test commands: %d steps\n", g_nsteps);
}

static void script_apply(ChiakiControllerState *st)
{
	script_load();
	if (!g_nsteps) return;
	DWORD now = GetTickCount();
	if (g_cur < 0 || now >= g_step_end)
	{
		g_cur++;
		if (g_cur >= g_nsteps) { g_nsteps = 0; g_cur = -1; return; }
		g_step_end = now + g_steps[g_cur].ms;
	}
	const Step &s = g_steps[g_cur];
	st->buttons |= s.buttons;
	if (s.l2) st->l2_state = s.l2;
	if (s.r2) st->r2_state = s.r2;
	if (s.stick) { if (s.lx || s.ly) { st->left_x = s.lx; st->left_y = s.ly; } if (s.rx || s.ry) { st->right_x = s.rx; st->right_y = s.ry; } }
}

static DWORD WINAPI pad_thread(void *)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1"); // sensors and effects over Bluetooth too
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "0");
	if (!SDL_Init(SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
	chiaki_controller_state_set_idle(&g_state);
	g_sent = g_state;
	memset(g_touch_ids, -1, sizeof(g_touch_ids));
	chiaki_orientation_tracker_init(&g_orient);
	chiaki_accel_new_zero_set_inactive(&g_accel_zero, false);
	chiaki_accel_new_zero_set_inactive(&g_real_accel, true);
	while (g_run)
	{
		SDL_Event e;
		// wakes up as soon as input arrives: no fixed wait on the button -> PS5 path
		if (SDL_WaitEventTimeout(&e, 10))
		{
			handle(e);
			while (SDL_PollEvent(&e)) handle(e);
		}
		nav_tick();
		ChiakiControllerState out = g_state;
		if (ui_captures_pad()) chiaki_controller_state_set_idle(&out); // the pad belongs to the UI
		else profile_apply(&out);
		script_apply(&out);
		ChiakiSession *ses = g_session;
		if (ses && !chiaki_controller_state_equals(&out, &g_sent))
		{
			chiaki_session_set_controller_state(ses, &out);
			g_sent = out;
		}
	}
	close_pad();
	SDL_Quit();
	return 0;
}

bool pad_init()
{
	g_run = true;
	g_thread = CreateThread(nullptr, 0, pad_thread, nullptr, 0, nullptr);
	return g_thread != nullptr;
}

void pad_attach(ChiakiSession *session)
{
	g_session = session;
	chiaki_controller_state_set_idle(&g_sent);
	if (!session)
	{
		g_haptic_active = false;
		rumble(0, 0);
		static const uint8_t off[10] = { 0 };
		trigger_effects(0x05, off, 0x05, off);
	}
}

const char *pad_name() { return g_name; }

void pad_fini()
{
	if (!g_thread) return;
	g_run = false;
	WaitForSingleObject(g_thread, 2000);
	CloseHandle(g_thread); g_thread = nullptr;
}

void pad_event(ChiakiEvent *ev)
{
	switch (ev->type)
	{
	case CHIAKI_EVENT_RUMBLE: rumble(ev->rumble.left, ev->rumble.right); break;
	case CHIAKI_EVENT_TRIGGER_EFFECTS:
		trigger_effects(ev->trigger_effects.type_left, ev->trigger_effects.left, ev->trigger_effects.type_right, ev->trigger_effects.right);
		break;
	case CHIAKI_EVENT_LED_COLOR:
		AcquireSRWLockShared(&g_pad_lock);
		if (g_pad) SDL_SetGamepadLED(g_pad, ev->led_state[0], ev->led_state[1], ev->led_state[2]);
		ReleaseSRWLockShared(&g_pad_lock);
		break;
	case CHIAKI_EVENT_PLAYER_INDEX:
		AcquireSRWLockShared(&g_pad_lock);
		if (g_pad) SDL_SetGamepadPlayerIndex(g_pad, ev->player_index);
		ReleaseSRWLockShared(&g_pad_lock);
		break;
	case CHIAKI_EVENT_MOTION_RESET: reset_motion(); break;
	case CHIAKI_EVENT_HAPTIC_INTENSITY:
	{
		uint8_t r = ev->intensity == Off ? 0x0F : ev->intensity == Strong ? 0x00 : ev->intensity == Medium ? 0x02 : 0x03;
		g_rumble_mult = ev->intensity == Off ? 0.f : ev->intensity == Strong ? 1.f : ev->intensity == Medium ? 0.5f : 0.33f;
		g_intensity = (g_intensity & 0xF0) | r;
		break;
	}
	case CHIAKI_EVENT_TRIGGER_INTENSITY:
	{
		uint8_t t = ev->intensity == Off ? 0xF0 : ev->intensity == Strong ? 0x00 : ev->intensity == Medium ? 0x60 : 0x90;
		g_intensity = (g_intensity & 0x0F) | t;
		break;
	}
	default: break;
	}
}

// PS5 haptics arrive as 3 kHz stereo audio (left/right). Without the DualSense USB audio channel
// they are translated into classic rumble, like chiaki-ng does with "Rumble haptics" Normal.
void pad_haptics(uint8_t *buf, size_t size)
{
	if (!size || size % 4 || g_rumble_mult <= 0.f || !g_set.haptics_rumble) return;
	size_t n = size / 4;
	uint32_t suml = 0, sumr = 0;
	for (size_t i = 0; i < n; i++)
	{
		int16_t l, r;
		memcpy(&l, buf + i * 4, 2); memcpy(&r, buf + i * 4 + 2, 2);
		suml += (uint32_t)abs(l) * 2; sumr += (uint32_t)abs(r) * 2;
	}
	uint32_t l = suml / n, r = sumr / n;
	l = l > 100 ? l : 0; r = r > 100 ? r : 0;
	if (!l && !r)
	{
		if (g_haptic_active) { rumble(0, 0); g_haptic_active = false; }
		return;
	}
	l = (uint32_t)(l * g_rumble_mult); r = (uint32_t)(r * g_rumble_mult);
	if (l > 65535) l = 65535;
	if (r > 65535) r = 65535;
	g_haptic_active = true;
	rumble((uint8_t)(l >> 8), (uint8_t)(r >> 8));
}

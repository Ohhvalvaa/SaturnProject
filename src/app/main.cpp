// SaturnProject: one exe to play your PS5 on a Windows PC.
// Home -> (PS5 wake-up) -> full screen game with audio and pad -> exit with the PS5 in rest mode.
// With "Quick start" (default) the Home screen is skipped: it connects right away and leaving the game closes the app.
// Command line options: --home (show the Home screen even with quick start), --windowed.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "app.h"
#include "settings.h"
#include "console.h"
#include "ui.h"
#include "steam.h"
#include <objbase.h>
#include <timeapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#define WM_APP_CONNECTED (WM_APP + 1)
#define WM_APP_SESSION_QUIT (WM_APP + 2)
#define WM_APP_AWAKE (WM_APP + 3)       // wParam: 1 = PS5 ready, 0 = no answer
#define WM_APP_CONSOLE_STATE (WM_APP + 4)

static ChiakiLog g_log;
static ChiakiSession g_session;
static HWND g_hwnd;
static FILE *g_logfile;
static SaturnConsole g_console;
static bool g_has_console;
static bool g_session_running, g_connected;
static volatile bool g_cancel_wake;
static HANDLE g_wake_thread;
static ChiakiQuitReason g_quit_reason;
static double g_orig_hz = 60;
static bool g_hz_changed;
static bool g_force_home, g_force_window, g_applied_full;

static void log_cb(ChiakiLogLevel level, const char *msg, void *)
{
	if (!(level & (CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO))) return;
	if (g_logfile) { fprintf(g_logfile, "%s\n", msg); fflush(g_logfile); }
}

// ---------- console ----------
static void load_console()
{
	g_has_console = saturn_console_load(&g_console);
	if (g_has_console) ui_set_console(g_console.registered.server_nickname, g_console.host, CS_UNKNOWN, true);
	else ui_set_console(nullptr, nullptr, CS_UNKNOWN, false);
}

// PS5 state on the Home screen, every 4 s
static DWORD WINAPI state_thread(void *)
{
	for (;;)
	{
		if (g_has_console && (ui_screen() == SCR_HOME))
		{
			int st = console_state(g_console.host);
			PostMessageW(g_hwnd, WM_APP_CONSOLE_STATE, (WPARAM)(st + 10), 0);
		}
		Sleep(4000);
	}
}

// Steam launcher: if it dies (Steam "Stop") the app closes as with Alt+F4
static DWORD WINAPI launcher_watch(void *p)
{
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)(uintptr_t)p);
	if (!h) return 0;
	WaitForSingleObject(h, INFINITE);
	CloseHandle(h);
	PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
	return 0;
}

// wake-up: up to 90 s (a PS5 just put in rest mode stays silent on the network for ~30 s)
static DWORD WINAPI wake_thread(void *)
{
	bool ok = false;
	for (int i = 0; i < 45 && !g_cancel_wake; i++)
	{
		int st = console_state(g_console.host);
		if (st == CS_READY) { ok = true; break; }
		if (!g_set.wake) { ok = st == CS_READY; if (!ok) break; }
		ui_set_status(st == CS_STANDBY ? T(L"Accensione della PS5…", L"Waking up the PS5…") : T(L"Ricerca della PS5…", L"Looking for the PS5…"));
		console_wakeup(g_console);
		for (int k = 0; k < 20 && !g_cancel_wake; k++) Sleep(100);
	}
	PostMessageW(g_hwnd, WM_APP_AWAKE, ok ? 1 : 0, 0);
	return 0;
}

// ---------- 120 Hz display while playing ----------
static void display_for_game()
{
	DEVMODEW dm = { sizeof(dm) };
	EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
	g_orig_hz = dm.dmDisplayFrequency;
	double hz = g_orig_hz;
	// CDS_FULLSCREEN: Windows restores the previous refresh rate by itself even if the app crashes
	// with variable refresh the display stays at its maximum rate (e.g. 144 Hz): frames reach the screen at their own time
	if (g_set.hz120 && !g_set.vrr && g_set.fullscreen && !g_force_window && hz != 120)
	{
		DEVMODEW t = dm; t.dmDisplayFrequency = 120; t.dmFields = DM_DISPLAYFREQUENCY | DM_PELSWIDTH | DM_PELSHEIGHT;
		if (ChangeDisplaySettingsExW(nullptr, &t, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL) { hz = 120; g_hz_changed = true; }
	}
	video_set_refresh(hz);
}

static void display_restore()
{
	if (g_hz_changed) { ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr); g_hz_changed = false; }
	video_set_refresh(g_orig_hz);
}

// ---------- session ----------
static void event_cb(ChiakiEvent *ev, void *)
{
	switch (ev->type)
	{
	case CHIAKI_EVENT_CONNECTED: PostMessageW(g_hwnd, WM_APP_CONNECTED, 0, 0); break;
	case CHIAKI_EVENT_QUIT:
		if (g_logfile) fprintf(g_logfile, "Session closed: %s\n", chiaki_quit_reason_string(ev->quit.reason));
		g_quit_reason = ev->quit.reason;
		PostMessageW(g_hwnd, WM_APP_SESSION_QUIT, 0, 0);
		break;
	default: pad_event(ev); break;
	}
}

static void haptics_cb(uint8_t *buf, size_t size, void *) { pad_haptics(buf, size); }

static bool start_session()
{
	ChiakiConnectInfo ci = {};
	ci.ps5 = true;
	ci.host = g_console.host;
	memcpy(ci.regist_key, g_console.registered.rp_regist_key, sizeof(ci.regist_key));
	memcpy(ci.morning, g_console.registered.rp_key, sizeof(ci.morning));
	int res = g_set.resolution;
	ChiakiVideoResolutionPreset rp = res <= 360 ? CHIAKI_VIDEO_RESOLUTION_PRESET_360p : res <= 540 ? CHIAKI_VIDEO_RESOLUTION_PRESET_540p
		: res <= 720 ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	chiaki_connect_video_profile_preset(&ci.video_profile, rp, g_set.fps == 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
	if (res == 1440) { ci.video_profile.width = 2560; ci.video_profile.height = 1440; }
	if (res == 2160) { ci.video_profile.width = 3840; ci.video_profile.height = 2160; }
	ci.video_profile.codec = g_set.codec ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
	ci.video_profile.bitrate = settings_bitrate();
	ci.video_profile_auto_downgrade = true;
	ci.enable_dualsense = true;
	memcpy(ci.psn_account_id, g_console.psn_account_id, sizeof(ci.psn_account_id));
	ci.packet_loss_max = 0.05;
	ci.enable_idr_on_fec_failure = true;
	if (chiaki_session_init(&g_session, &ci, &g_log) != CHIAKI_ERR_SUCCESS) return false;
	chiaki_session_set_event_cb(&g_session, event_cb, nullptr);
	chiaki_session_set_video_sample_cb(&g_session, video_sample, nullptr);
	audio_attach(&g_session, &g_log);
	ChiakiAudioSink hs = {}; hs.frame_cb = haptics_cb;
	chiaki_session_set_haptics_sink(&g_session, &hs);
	if (!video_begin(g_set.codec != 0)) { audio_fini(); chiaki_session_fini(&g_session); return false; }
	if (chiaki_session_start(&g_session) != CHIAKI_ERR_SUCCESS) { video_end(); audio_fini(); chiaki_session_fini(&g_session); return false; }
	g_session_running = true;
	g_connected = false;
	pad_attach(&g_session);
	fprintf(stderr, "Session: %dx%d %d fps %s %d kbit/s\n", ci.video_profile.width, ci.video_profile.height, ci.video_profile.max_fps,
		g_set.codec ? "H.265" : "H.264", ci.video_profile.bitrate);
	return true;
}

static void stop_session(bool goto_bed)
{
	if (!g_session_running) return;
	pad_attach(nullptr);
	video_end();
	if (goto_bed && g_connected)
	{
		ui_set_screen(SCR_CONNECT);
		ui_set_status(T(L"La PS5 va in riposo…", L"Putting the PS5 to rest…"));
		chiaki_session_goto_bed(&g_session);
		Sleep(1500);
	}
	chiaki_session_stop(&g_session);
	chiaki_session_join(&g_session);
	chiaki_session_fini(&g_session);
	audio_fini();
	g_session_running = false;
	g_connected = false;
	display_restore();
}

static const wchar_t *quit_message(ChiakiQuitReason r)
{
	switch (r)
	{
	case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE: return T(L"La PS5 \u00E8 gi\u00E0 in uso in Riproduzione remota. Riprova tra qualche secondo.", L"The PS5 is already in use for Remote Play. Try again in a few seconds.");
	case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
	case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED: return T(L"La PS5 ha rifiutato la connessione. Se \u00E8 stata riassociata, associala di nuovo qui.", L"The PS5 refused the connection. If it was paired again elsewhere, pair it again here.");
	case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH: return T(L"La Riproduzione remota della PS5 si \u00E8 bloccata. Riprova.", L"Remote Play crashed on the PS5. Try again.");
	case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH: return T(L"Versione della PS5 non compatibile.", L"Incompatible PS5 version.");
	case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN: return T(L"La PS5 si \u00E8 spenta o \u00E8 andata in riposo.", L"The PS5 shut down or went to rest.");
	case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED: return T(L"La PS5 ha chiuso la connessione.", L"The PS5 closed the connection.");
	default: return T(L"Connessione con la PS5 persa.", L"Connection to the PS5 lost.");
	}
}

// ---------- window ----------
static void apply_window_mode()
{
	bool full = g_set.fullscreen && !g_force_window;
	g_applied_full = full;
	HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
	RECT r = mi.rcMonitor;
	if (full)
	{
		SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(g_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
	else
	{
		int w = (r.right - r.left) * 2 / 3, h = w * 9 / 16;
		RECT wr = { 0, 0, w, h };
		AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
		SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPos(g_hwnd, HWND_NOTOPMOST, r.left + (r.right - r.left - (wr.right - wr.left)) / 2, r.top + (r.bottom - r.top - (wr.bottom - wr.top)) / 2,
			wr.right - wr.left, wr.bottom - wr.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
}

static void play()
{
	if (!g_has_console || g_session_running || g_wake_thread) return;
	ui_set_screen(SCR_CONNECT);
	ui_set_status(T(L"Ricerca della PS5…", L"Looking for the PS5…"));
	g_cancel_wake = false;
	g_wake_thread = CreateThread(nullptr, 0, wake_thread, nullptr, 0, nullptr);
}

static void after_game(bool error, const wchar_t *msg)
{
	if (g_set.quick_start && !g_force_home && !error) { DestroyWindow(g_hwnd); return; }
	ui_set_screen(SCR_HOME);
	if (msg) ui_error(msg);
}

static bool g_closing;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_APP_UI:
		switch ((UiCmd)w)
		{
		case CMD_PLAY: play(); break;
		case CMD_CANCEL_CONNECT:
			g_cancel_wake = true;
			if (g_session_running) stop_session(false);
			ui_set_screen(SCR_HOME);
			break;
		case CMD_LEAVE_REST: stop_session(true); after_game(false, nullptr); break;
		case CMD_LEAVE_ON: stop_session(false); after_game(false, nullptr); break;
		case CMD_QUIT_APP: PostMessageW(h, WM_CLOSE, 0, 0); break;
		case CMD_SETTINGS_CHANGED:
			if ((g_set.fullscreen && !g_force_window) != g_applied_full) apply_window_mode();
			break;
		case CMD_PAIRED: load_console(); break;
		case CMD_FORGET:
			DeleteFileA(saturn_console_path());
			load_console();
			break;
		}
		return 0;
	case WM_APP_AWAKE:
		if (g_wake_thread) { WaitForSingleObject(g_wake_thread, 1000); CloseHandle(g_wake_thread); g_wake_thread = nullptr; }
		if (g_cancel_wake) return 0;
		if (!w)
		{
			ui_set_screen(SCR_HOME);
			ui_error(g_set.wake ? T(L"La PS5 non risponde. \u00C8 accesa o in riposo, sulla stessa rete? (Se \u00E8 spenta del tutto va accesa a mano.)", L"The PS5 does not respond. Is it on or in rest mode, on the same network? (If fully off, turn it on by hand.)")
				: T(L"La PS5 non \u00E8 accesa (accensione automatica disattivata nelle impostazioni).", L"The PS5 is not on (automatic wake is off in settings)."));
			return 0;
		}
		ui_set_status(T(L"Connessione…", L"Connecting…"));
		display_for_game();
		if (!start_session()) { display_restore(); ui_set_screen(SCR_HOME); ui_error(T(L"Connessione alla PS5 non riuscita.", L"Could not connect to the PS5.")); }
		return 0;
	case WM_APP_CONNECTED:
		g_connected = true;
		ui_set_screen(SCR_GAME);
		ui_toast(T(L"Menu: tasto Mute del DualSense, F1 o Esc", L"Menu: DualSense Mute button, F1 or Esc"), 5);
		return 0;
	case WM_APP_SESSION_QUIT:
	{
		if (!g_session_running) return 0;
		ChiakiQuitReason r = g_quit_reason;
		bool was_connected = g_connected;
		stop_session(false);
		if (g_closing) return 0;
		// the PS5 was turned off/put to rest from its own menu with quick start: the app closes
		bool shutdown = r == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN && was_connected;
		after_game(!shutdown, quit_message(r));
		return 0;
	}
	case WM_APP_CONSOLE_STATE:
		if (g_has_console) ui_set_console(g_console.registered.server_nickname, g_console.host, (int)w - 10, true);
		return 0;
	case WM_SYSKEYDOWN:
		if (w == VK_RETURN && (l & (1 << 29))) { g_set.fullscreen = !g_set.fullscreen; settings_save(); apply_window_mode(); return 0; }
		break;
	case WM_KEYDOWN:
		if (w == VK_F12) { video_screenshot(); return 0; }
		ui_key(w); return 0;
	case WM_CHAR: ui_char((wchar_t)w); return 0;
	case WM_MOUSEMOVE: ui_mouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), false, 0); return 0;
	case WM_LBUTTONUP: ui_mouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), true, 0); return 0;
	case WM_MOUSEWHEEL: { POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p); ui_mouse(p.x, p.y, false, GET_WHEEL_DELTA_WPARAM(w)); return 0; }
	case WM_SETCURSOR:
		if (LOWORD(l) == HTCLIENT) { SetCursor(ui_captures_pad() || ui_menu_open() ? LoadCursorW(nullptr, IDC_ARROW) : nullptr); return TRUE; }
		break;
	case WM_SIZE: video_resize(LOWORD(l), HIWORD(l)); return 0;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
	case WM_CLOSE:
		g_closing = true;
		g_cancel_wake = true;
		if (g_session_running) stop_session(g_set.rest_on_exit);
		DestroyWindow(h);
		return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
	timeBeginPeriod(1);
	g_force_home = cmd && wcsstr(cmd, L"--home");
	g_force_window = cmd && wcsstr(cmd, L"--windowed");

	// started from Steam: relaunch outside Steam (real DualSense, no overlay) and this process waits
	{ int code = 0; if (steam_relaunch_detached(cmd, &code)) return code; }

	// single instance: if already open, bring it to the front
	HANDLE mutex = CreateMutexW(nullptr, TRUE, L"SaturnProject.SingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		HWND other = FindWindowW(L"SaturnProject", nullptr);
		if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
		return 0;
	}

	settings_load();
	char logpath[MAX_PATH]; snprintf(logpath, sizeof(logpath), "%s\\saturn.log", settings_dir());
	// one log: %LOCALAPPDATA%\SaturnProject\saturn.log (module messages on stderr too)
	if (freopen(logpath, "w", stderr)) g_logfile = stderr;
	fprintf(stderr, "SaturnProject %ls\n", SATURN_VERSION_W);
	chiaki_log_init(&g_log, CHIAKI_LOG_ALL, log_cb, nullptr);
	chiaki_lib_init();
	console_set_log(&g_log);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"SaturnProject";
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassW(&wc);
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SaturnProject", WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr, inst, nullptr);
	BOOL dark = TRUE; DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark)); // dark title bar in windowed mode
	apply_window_mode();
	SetForegroundWindow(g_hwnd);
	ui_set_hwnd(g_hwnd);
	{
		// steady performance: no Windows power throttling (EcoQoS) and a precise timer (high priority only on request)
		if (getenv("SATURN_HIGH")) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
		SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
		timeBeginPeriod(1);
	}

	{
		DEVMODEW dm = { sizeof(dm) };
		EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
		g_orig_hz = dm.dmDisplayFrequency;
	}
	if (!video_init(g_hwnd))
	{
		MessageBoxW(g_hwnd, T(L"Serve una scheda video con decodifica hardware (DirectX 11).", L"A graphics card with hardware decoding (DirectX 11) is required."), L"SaturnProject", MB_ICONERROR);
		return 1;
	}
	video_set_refresh(g_orig_hz);
	pad_init();
	load_console();
	CloseHandle(CreateThread(nullptr, 0, state_thread, nullptr, 0, nullptr));
	if (DWORD lp = steam_launcher_pid(cmd)) CloseHandle(CreateThread(nullptr, 0, launcher_watch, (void *)(uintptr_t)lp, 0, nullptr));
	if (g_has_console && g_set.quick_start && !g_force_home) play();
	else ui_set_screen(SCR_HOME);

	MSG m;
	DWORD last = GetTickCount();
	VideoStats prev = video_stats();
	for (;;)
	{
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
		bool quit = false;
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) quit = true; TranslateMessage(&m); DispatchMessageW(&m); }
		if (quit) break;
		ui_tick();
		DWORD now = GetTickCount();
		if (now - last >= 1000)
		{
			double dt = (now - last) / 1000.0;
			last = now;
			VideoStats s = video_stats();
			if (g_session_running && g_connected)
			{
				UiStats u = {};
				u.fps_shown = (s.presented - prev.presented) / dt;
				u.fps_source = (s.real_shown - prev.real_shown) / dt;
				u.fps_dups = (s.dups - prev.dups) / dt;
				u.fps_generated = (s.generated - prev.generated) / dt;
				u.fg_ms = s.fruc_ms; u.decode_ms = s.decode_ms;
				u.rtt_ms = g_session.rtt_us / 1000.0;
				u.mbit = (s.bytes - prev.bytes) * 8.0 / dt / 1e6;
				u.src_w = s.src_w; u.src_h = s.src_h; u.out_w = s.out_w; u.out_h = s.out_h; u.refresh = (int)(s.refresh + 0.5);
				u.vsr = g_set.vsr; u.fg = s.engine && s.engine[0]; u.h265 = g_set.codec != 0;
				u.dropped = s.dropped; u.errors = s.errors; u.fg_engine = s.engine;
				u.av_auto = video_av_delay_ms();
				u.repeats = (s.vsyncs - prev.vsyncs) / dt; if (u.repeats < 0) u.repeats = 0;
				ui_set_stats(u);
				if (g_logfile) fprintf(g_logfile, "video: %ld decoded/s, %ld duplicates/s, %ld generated/s, %ld shown/s | skipped %ld, errors %ld | decode %.2f ms, generation %.2f ms, %.1f Mbit/s, RTT %.1f ms | display: %ld repeats/s\n",
					s.decoded - prev.decoded, s.dups - prev.dups, s.generated - prev.generated, s.presented - prev.presented, s.dropped, s.errors,
					s.decode_ms, s.fruc_ms, u.mbit, u.rtt_ms, s.vsyncs - prev.vsyncs);
			}
			prev = s;
		}
	}
	g_cancel_wake = true;
	if (g_session_running) stop_session(g_set.rest_on_exit);
	pad_fini();
	video_fini();
	display_restore();
	timeEndPeriod(1);
	if (g_logfile) fclose(g_logfile);
	ReleaseMutex(mutex);
	return 0;
}

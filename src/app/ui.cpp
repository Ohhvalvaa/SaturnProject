// SaturnProject user interface: Direct2D + DirectWrite on the swapchain image.
// Palette (branding/palette.md): black, graphite, greys, silver, white. No bright colors.
// All sizes are designed for 1080p and multiplied by S = height/1080.
#include "ui.h"
#include "app.h"
#include "settings.h"
#include "console.h"
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shellapi.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

// ---------- colors ----------
static const D2D1_COLOR_F C_BLACK = { 0, 0, 0, 1 };
static D2D1_COLOR_F rgb(unsigned v, float a = 1.f) { return { ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a }; }
#define NERO 0x0A0A0A
#define GRAFITE 0x141414
#define GRIGIO_SCURO 0x262626
#define GRIGIO 0x5C5C5C
#define GRIGIO_CHIARO 0x9A9A9A
#define ARGENTO 0xC8C8C8
#define BIANCO 0xF5F5F5

// ---------- Direct2D ----------
static ID2D1Factory1 *g_f;
static ID2D1Device *g_d2dev;
static ID2D1DeviceContext *g_dc;
static IDWriteFactory *g_dw;
static ID2D1SolidColorBrush *g_br;
static ID2D1Bitmap *g_logo;
static ID2D1PathGeometry *g_arc;
struct Target { ID3D11Texture2D *tex; ID2D1Bitmap1 *bmp; };
static Target g_targets[4];
static ID2D1Bitmap1 *g_cache; // menu/statistics already drawn (see render_overlay_cache)
static float S = 1.f, W = 1920, H = 1080;
static float g_fmt_scale = -1;
enum { F_HUGE, F_TITLE, F_H1, F_ITEM, F_BODY, F_SMALL, F_TINY, F_NUM, F_COUNT };
static IDWriteTextFormat *g_fmt[F_COUNT];

// ---------- UI state (protected by g_lock) ----------
static CRITICAL_SECTION g_lock;
static HWND g_hwnd;
static UiScreen g_scr = SCR_HOME, g_settings_back = SCR_HOME;
static bool g_menu;
static int g_home_sel, g_menu_sel, g_set_sel, g_pair_sel;
static float g_set_scroll;
static char g_cname[64], g_chost[64];
static int g_cstate = CS_UNKNOWN;
static bool g_paired;
static wchar_t g_status[160];
static wchar_t g_toast[160]; static double g_toast_until;
static wchar_t g_err[200]; static double g_err_until;
static UiStats g_stats; static bool g_have_stats;
static bool g_pad_input; // last command came from the pad (hints with PS symbols)
static int g_confirm = -1; // entry waiting for confirmation (e.g. forget PS5)
static double g_t0;

struct Hit { D2D1_RECT_F r; int screen, idx; };
static Hit g_hits[64]; static int g_nhits;
static Hit g_hits_draw[64]; static int g_nhits_draw;

static double now_s()
{
	static LARGE_INTEGER fq; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
	LARGE_INTEGER t; QueryPerformanceCounter(&t);
	return (double)t.QuadPart / fq.QuadPart;
}

static void post(UiCmd c, LPARAM l = 0) { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UI, c, l); }

// ---------- pairing: state ----------
static FoundConsole g_found[8]; static int g_nfound; static bool g_scanning;
static int g_found_sel;                 // index in g_found, == g_nfound -> manual address
static wchar_t g_ip[64], g_account[32], g_pin[16];
static bool g_pairing, g_psn_wait, g_psn_busy;
static wchar_t g_pair_msg[220];

static DWORD WINAPI scan_thread(void *)
{
	FoundConsole f[8];
	int n = console_discover(f, 8);
	EnterCriticalSection(&g_lock);
	memcpy(g_found, f, sizeof(f)); g_nfound = n; g_scanning = false;
	if (g_found_sel > n) g_found_sel = n;
	// the first PS5 found
	for (int i = 0; i < n; i++) if (f[i].ps5) { g_found_sel = i; break; }
	LeaveCriticalSection(&g_lock);
	return 0;
}

static void start_scan()
{
	if (g_scanning) return;
	g_scanning = true;
	CloseHandle(CreateThread(nullptr, 0, scan_thread, nullptr, 0, nullptr));
}

static DWORD WINAPI pair_thread(void *)
{
	char host[64], acc[32], pin[16];
	EnterCriticalSection(&g_lock);
	if (g_found_sel < g_nfound) strcpy(host, g_found[g_found_sel].host); else snprintf(host, 64, "%ls", g_ip);
	snprintf(acc, 32, "%ls", g_account); snprintf(pin, 16, "%ls", g_pin);
	LeaveCriticalSection(&g_lock);
	SaturnConsole c; wchar_t err[200] = {};
	bool ok = console_register(host, acc, pin, &c, err, 200);
	EnterCriticalSection(&g_lock);
	g_pairing = false;
	if (ok)
	{
		swprintf(g_pair_msg, 220, T(L"Associata: %hs", L"Paired: %hs"), c.registered.server_nickname);
		g_scr = SCR_HOME; g_home_sel = 0;
		swprintf(g_toast, 160, T(L"PS5 associata: %hs", L"PS5 paired: %hs"), c.registered.server_nickname);
		g_toast_until = now_s() + 5;
		post(CMD_PAIRED);
	}
	else wcsncpy(g_pair_msg, err, 219);
	LeaveCriticalSection(&g_lock);
	return 0;
}

static char g_redirect[2048];
static DWORD WINAPI psn_thread(void *)
{
	char b64[32] = {}; wchar_t err[200] = {};
	bool ok = psn_account_from_redirect(g_redirect, b64, sizeof(b64), err, 200);
	EnterCriticalSection(&g_lock);
	g_psn_busy = false;
	if (ok) { swprintf(g_account, 32, L"%hs", b64); swprintf(g_pair_msg, 220, T(L"Account PSN letto: %hs", L"PSN account read: %hs"), b64); g_pair_sel = 4; }
	else wcsncpy(g_pair_msg, err, 219);
	LeaveCriticalSection(&g_lock);
	return 0;
}

// ---------- settings: list of entries ----------
struct Opt { int v; const wchar_t *it, *en; };
enum Kind { K_BOOL, K_CHOICE, K_RANGE, K_ACTION };
enum Action { A_NONE, A_FORGET, A_FOLDER, A_DEFAULTS, A_STEAM, A_RESUME, A_LEAVE_REST, A_LEAVE_ON, A_SHOT, A_KEYS_RESET };
struct SItem
{
	int section; const wchar_t *it, *en, *dit, *den;
	Kind kind; bool *b; int *i; const Opt *opts; int nopts; int lo, hi, step; const wchar_t *unit; Action act;
};
static const Opt O_LANG[] = { { LANG_AUTO, L"Automatica", L"Automatic" }, { LANG_IT, L"Italiano", L"Italiano" }, { LANG_EN, L"English", L"English" } };
static const Opt O_RES[] = { { 540, L"540p", L"540p" }, { 720, L"720p", L"720p" }, { 1080, L"1080p", L"1080p" }, { 1440, L"1440p", L"1440p" }, { 2160, L"4K (2160p)", L"4K (2160p)" } };
static const Opt O_FPS[] = { { 30, L"30 fps", L"30 fps" }, { 60, L"60 fps", L"60 fps" } };
static const Opt O_CODEC[] = { { 0, L"H.264", L"H.264" }, { 1, L"H.265 (HEVC)", L"H.265 (HEVC)" } };
static const Opt O_BITRATE[] = { { 0, L"Automatico", L"Automatic" }, { 10000, L"10 Mbit/s", L"10 Mbit/s" }, { 15000, L"15 Mbit/s", L"15 Mbit/s" },
	{ 20000, L"20 Mbit/s", L"20 Mbit/s" }, { 30000, L"30 Mbit/s", L"30 Mbit/s" }, { 40000, L"40 Mbit/s", L"40 Mbit/s" }, { 50000, L"50 Mbit/s", L"50 Mbit/s" },
	{ 60000, L"60 Mbit/s", L"60 Mbit/s" }, { 80000, L"80 Mbit/s", L"80 Mbit/s" }, { 100000, L"100 Mbit/s", L"100 Mbit/s" } };
static const Opt O_FG[] = { { FG_SMOOTH, L"Automatica", L"Automatic" }, { FG_X2, L"Leggera (×2)", L"Light (×2)" }, { FG_AUTO, L"Massima", L"Maximum" }, { FG_OFF, L"Spenta", L"Off" } };
static const Opt O_ENGINE[] = { { FGE_DLSS, L"NVIDIA DLSS", L"NVIDIA DLSS" }, { FGE_FSR, L"AMD FSR 3", L"AMD FSR 3" }, { FGE_NVOF, L"Flusso ottico", L"Optical flow" }, { FGE_FRUC, L"NVIDIA FRUC", L"NVIDIA FRUC" } };

// panel tabs (on the right): the first one only while playing
enum Tab { T_GAME, T_PICTURE, T_STREAM, T_AUDIO, T_PAD, T_KEYS, T_SYSTEM, T_COUNT };
static const wchar_t *TABS_IT[] = { L"Gioco", L"Immagine", L"Flusso", L"Audio", L"Pad", L"Tasti", L"Sistema" };
static const wchar_t *TABS_EN[] = { L"Game", L"Picture", L"Stream", L"Audio", L"Pad", L"Buttons", L"System" };
static const Opt O_CURVE[] = { { 0, L"Lineare", L"Linear" }, { 1, L"Morbida", L"Soft" }, { 2, L"Rapida", L"Fast" } };
static const Opt O_BTN[] = { { 0, L"\u2715", L"\u2715" }, { 1, L"\u25CB", L"\u25CB" }, { 2, L"\u25A1", L"\u25A1" }, { 3, L"\u25B3", L"\u25B3" }, { 4, L"L1", L"L1" }, { 5, L"R1", L"R1" },
	{ 6, L"L2", L"L2" }, { 7, L"R2", L"R2" }, { 8, L"L3", L"L3" }, { 9, L"R3", L"R3" }, { 10, L"Options", L"Options" }, { 11, L"Create", L"Create" } };
#define N_OPTS(a) (int)(sizeof(a) / sizeof(a[0]))

static SItem g_items[] = {
	{ T_GAME, L"Riprendi", L"Resume", L"Torna al gioco.", L"Back to the game.", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_RESUME },
	{ T_GAME, L"Statistiche sul gioco", L"Statistics over the game", L"Pannello con frequenza, ritmo e rete in alto a destra mentre giochi. F2 in gioco.", L"Frame rate, pacing and network panel in the top right while playing. F2 while playing.", K_BOOL, &g_set.stats },
	{ T_GAME, L"Modalit\u00E0 competitiva", L"Competitive mode", L"Generazione fotogrammi spenta e latenza minima: per i giochi online dove conta la reattivit\u00E0.", L"Frame generation off and minimum latency: for online games where responsiveness matters.", K_BOOL, &g_set.competitive },
	{ T_GAME, L"Foto dello schermo", L"Screenshot", L"Salva l'immagine del gioco in Immagini \u203A SaturnProject. In gioco anche col tasto F12.", L"Saves the game picture in Pictures \u203A SaturnProject. Also F12 while playing.", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_SHOT },
	{ T_GAME, L"Esci e metti la PS5 in riposo", L"Leave and put the PS5 to rest", L"Chiude il gioco e manda la PS5 in modalità riposo.", L"Closes the session and puts the PS5 in rest mode.", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_LEAVE_REST },
	{ T_GAME, L"Esci lasciando la PS5 accesa", L"Leave with the PS5 on", L"Chiude il gioco, la PS5 resta accesa.", L"Closes the session, the PS5 stays on.", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_LEAVE_ON },
	{ T_PICTURE, L"Generazione fotogrammi", L"Frame generation", L"Automatica (consigliata): 120 con i giochi a 60, 60 veri con quelli a 30. Crea immagini intermedie per un movimento più fluido (fino a 120 su uno schermo a 120 Hz). Aggiunge pochi millisecondi di ritardo.", L"Automatic (recommended): 120 with 60 fps games, true 60 with 30 fps games. Creates in-between frames for smoother motion (up to 120 on a 120 Hz display). Adds a few milliseconds of latency.", K_CHOICE, nullptr, &g_set.fg, O_FG, N_OPTS(O_FG) },
	{ T_PICTURE, L"Motore", L"Engine", L"DLSS: rete neurale NVIDIA, la qualità migliore (RTX 40 e successive). FSR 3: AMD, funziona su tutte le schede (NVIDIA, AMD, Intel). Flusso ottico: tutte le RTX. Se il motore scelto manca si passa da soli al successivo.", L"DLSS: NVIDIA neural network, best quality (RTX 40 and newer). FSR 3: AMD, works on any GPU (NVIDIA, AMD, Intel). Optical flow: any RTX. If the chosen engine is missing the next one is used.", K_CHOICE, nullptr, &g_set.fg_engine, O_ENGINE, N_OPTS(O_ENGINE) },
	{ T_PICTURE, L"Nitidezza", L"Sharpness", L"Rende pi\u00F9 netti i dettagli dell'immagine ingrandita, senza aloni sui contorni gi\u00E0 marcati.", L"Makes details of the upscaled picture crisper, without halos on edges that are already sharp.", K_RANGE, nullptr, &g_set.sharpen, nullptr, 0, 0, 100, 10, L"%" },
	{ T_PICTURE, L"Ombre", L"Shadows", L"Schiarisce i toni scuri lasciando invariate le luci: utile nei giochi molto bui.", L"Brightens dark tones while leaving highlights untouched: useful in very dark games.", K_RANGE, nullptr, &g_set.shadows, nullptr, 0, 0, 100, 10, L"%" },
	{ T_PICTURE, L"Saturazione", L"Saturation", L"Intensit\u00E0 dei colori (100% = come la PS5).", L"Color intensity (100% = as the PS5 sends it).", K_RANGE, nullptr, &g_set.saturation, nullptr, 0, 50, 150, 10, L"%" },
	{ T_PICTURE, L"RTX Video Super Resolution", L"RTX Video Super Resolution", L"Ingrandimento intelligente NVIDIA fino alla risoluzione dello schermo.", L"NVIDIA AI upscaling to the display resolution.", K_BOOL, &g_set.vsr },
	{ T_PICTURE, L"Schermo intero", L"Fullscreen", L"Senza bordi sul monitor principale. Alt+Invio cambia al volo.", L"Borderless on the main monitor. Alt+Enter toggles.", K_BOOL, &g_set.fullscreen },
	{ T_PICTURE, L"Frequenza variabile (FreeSync / G-Sync)", L"Variable refresh (FreeSync / G-Sync)", L"Ogni immagine va a schermo nel suo istante esatto e il monitor si adegua: movimento regolare anche con 62-64 fps o 120 generate. Serve un monitor FreeSync o G-Sync con G-SYNC attivo nel pannello NVIDIA (o FreeSync nel software AMD).", L"Every frame reaches the screen at its exact time and the monitor follows: smooth motion even at 62-64 fps or 120 generated. Needs a FreeSync or G-Sync monitor with G-SYNC enabled in the NVIDIA Control Panel (or FreeSync in AMD Software).", K_BOOL, &g_set.vrr },
	{ T_PICTURE, L"Schermo a 120 Hz in gioco", L"120 Hz display while playing", L"Durante il gioco porta lo schermo a 120 Hz (se lo supporta) e poi rimette la frequenza di prima.", L"Switches the display to 120 Hz while playing (if supported), then restores it.", K_BOOL, &g_set.hz120 },
	{ T_STREAM, L"Risoluzione", L"Resolution", L"Risoluzione del flusso. 1440p e 4K solo su PS5 che li supportano. Vale dalla prossima connessione.", L"Stream resolution. 1440p and 4K only on PS5 models that support them. Applies on next connection.", K_CHOICE, nullptr, &g_set.resolution, O_RES, N_OPTS(O_RES) },
	{ T_STREAM, L"Fotogrammi al secondo", L"Frame rate", L"60 consigliato: i giochi a 30 vengono riconosciuti da soli. Vale dalla prossima connessione.", L"60 recommended: 30 fps games are detected automatically. Applies on next connection.", K_CHOICE, nullptr, &g_set.fps, O_FPS, N_OPTS(O_FPS) },
	{ T_STREAM, L"Codifica", L"Codec", L"H.265 dà un'immagine migliore a parità di banda. Vale dalla prossima connessione.", L"H.265 gives a better picture at the same bitrate. Applies on next connection.", K_CHOICE, nullptr, &g_set.codec, O_CODEC, N_OPTS(O_CODEC) },
	{ T_STREAM, L"Bitrate", L"Bitrate", L"Automatico sceglie il massimo adatto alla risoluzione su rete locale.", L"Automatic picks the best value for the resolution on a local network.", K_CHOICE, nullptr, &g_set.bitrate, O_BITRATE, N_OPTS(O_BITRATE) },
	{ T_AUDIO, L"Volume", L"Volume", L"Volume del gioco.", L"Game volume.", K_RANGE, nullptr, &g_set.volume, nullptr, 0, 0, 100, 10, L"%" },
	{ T_AUDIO, L"Ritardo audio", L"Audio delay", L"Ritardo in più, oltre a quello automatico che con la generazione fotogrammi tiene l'audio insieme all'immagine.", L"Extra delay on top of the automatic one that keeps audio in sync with frame generation.", K_RANGE, nullptr, &g_set.audio_delay, nullptr, 0, 0, 200, 10, L" ms" },
	{ T_PAD, L"Vibrazione", L"Rumble", L"Vibrazione del controller.", L"Controller rumble.", K_BOOL, &g_set.rumble },
	{ T_PAD, L"Aptica come vibrazione", L"Haptics as rumble", L"Traduce l'aptica dei giochi PS5 in vibrazione.", L"Turns PS5 haptics into rumble.", K_BOOL, &g_set.haptics_rumble },
	{ T_PAD, L"Grilletti adattivi", L"Adaptive triggers", L"Resistenza di L2 e R2 dei giochi PS5 (DualSense).", L"L2/R2 resistance in PS5 games (DualSense).", K_BOOL, &g_set.adaptive_triggers },
	{ T_PAD, L"Zona morta degli stick", L"Stick dead zone", L"Movimenti pi\u00F9 piccoli di cos\u00EC vengono ignorati: alzala se il personaggio si muove da solo.", L"Movements smaller than this are ignored: raise it if the character drifts on its own.", K_RANGE, nullptr, &g_set.deadzone, nullptr, 0, 0, 30, 2, L"%" },
	{ T_PAD, L"Risposta degli stick", L"Stick response", L"Morbida: pi\u00F9 precisa nei piccoli movimenti (mira). Rapida: arriva prima al massimo.", L"Soft: more precise small movements (aiming). Fast: reaches full speed sooner.", K_CHOICE, nullptr, &g_set.stick_curve, O_CURVE, N_OPTS(O_CURVE) },
	{ T_SYSTEM, L"Avvio rapido", L"Quick start", L"All'apertura si collega subito alla PS5; uscendo dal gioco l'app si chiude.", L"Connects to the PS5 right away; leaving the game closes the app.", K_BOOL, &g_set.quick_start },
	{ T_SYSTEM, L"Accendi la PS5", L"Wake the PS5", L"Se la PS5 è in modalità riposo la accende.", L"Wakes the PS5 from rest mode.", K_BOOL, &g_set.wake },
	{ T_SYSTEM, L"Riposo all'uscita", L"Rest mode on exit", L"Uscendo dal gioco la PS5 torna in modalità riposo.", L"Puts the PS5 in rest mode when you leave.", K_BOOL, &g_set.rest_on_exit },
	{ T_SYSTEM, L"Lingua", L"Language", L"Lingua dell'interfaccia.", L"Interface language.", K_CHOICE, nullptr, &g_set.lang, O_LANG, N_OPTS(O_LANG) },
	{ T_SYSTEM, L"Cartella dei dati", L"Data folder", L"Apre la cartella con impostazioni e registro (saturn.log).", L"Opens the folder with settings and log (saturn.log).", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_FOLDER },
	{ T_SYSTEM, L"Ripristina le impostazioni", L"Reset settings", L"Rimette tutte le impostazioni ai valori iniziali.", L"Restores all settings to their defaults.", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_DEFAULTS },
	{ T_SYSTEM, L"Dimentica la PS5", L"Forget the PS5", L"Cancella l'associazione con la PS5 (servirà associarla di nuovo).", L"Removes the PS5 pairing (you will need to pair again).", K_ACTION, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, A_FORGET },
};
static const int N_ITEMS = N_OPTS(g_items);

static int opt_index(const SItem &it)
{
	for (int k = 0; k < it.nopts; k++) if (it.opts[k].v == *it.i) return k;
	return 0;
}

static void item_value(const SItem &it, wchar_t *out, int n)
{
	switch (it.kind)
	{
	case K_BOOL: wcsncpy(out, *it.b ? T(L"S\u00EC", L"On") : T(L"No", L"Off"), n); break;
	case K_CHOICE: { const Opt &o = it.opts[opt_index(it)]; wcsncpy(out, T(o.it, o.en), n); break; }
	case K_RANGE: swprintf(out, n, L"%d%s", *it.i, it.unit); break;
	default: out[0] = 0; break;
	}
}

static void changed() { settings_save(); post(CMD_SETTINGS_CHANGED); }

static void item_change(int idx, int dir) // dir: -1, +1, 0 = confirm (OK)
{
	SItem &it = g_items[idx];
	switch (it.kind)
	{
	case K_BOOL: *it.b = !*it.b; changed(); break;
	case K_CHOICE:
	{
		int k = opt_index(it) + (dir ? dir : 1);
		if (dir == 0 && k >= it.nopts) k = 0;
		if (k < 0 || k >= it.nopts) return;
		*it.i = it.opts[k].v; changed();
		break;
	}
	case K_RANGE:
	{
		int v = *it.i + (dir ? dir : 1) * it.step;
		if (dir == 0 && v > it.hi) v = it.lo;
		if (v < it.lo || v > it.hi) return;
		*it.i = v; changed();
		break;
	}
	case K_ACTION:
		if (dir != 0) return;
		if (it.act == A_RESUME) { g_menu = false; return; }
		if (it.act == A_SHOT) { g_menu = false; video_screenshot(); return; }
		if (it.act == A_LEAVE_REST) { g_menu = false; post(CMD_LEAVE_REST); return; }
		if (it.act == A_LEAVE_ON) { g_menu = false; post(CMD_LEAVE_ON); return; }
		if (it.act == A_FOLDER) ShellExecuteA(nullptr, "open", settings_dir(), nullptr, nullptr, SW_SHOWNORMAL);
		else if (g_confirm != idx) { g_confirm = idx; return; }
		else if (it.act == A_DEFAULTS) { g_set = Settings(); changed(); g_toast_until = now_s() + 3; wcscpy(g_toast, T(L"Impostazioni ripristinate", L"Settings restored")); }
		else if (it.act == A_FORGET) { post(CMD_FORGET); g_toast_until = now_s() + 3; wcscpy(g_toast, T(L"PS5 dimenticata", L"PS5 forgotten")); }
		g_confirm = -1;
		break;
	}
}

// ---------- menu: entries ----------
enum { H_PLAY, H_SETTINGS, H_PAIR, H_QUIT, H_COUNT };
enum { P_CONSOLE, P_IP, P_ACCOUNT, P_PSN, P_PIN, P_GO, P_COUNT };

static bool pair_manual() { return g_found_sel >= g_nfound; }
static bool pair_visible(int i) { return i != P_IP || pair_manual(); }

// ---------- drawing: helpers ----------
static void make_formats()
{
	if (g_fmt_scale == S) return;
	g_fmt_scale = S;
	for (auto &f : g_fmt) release(f);
	struct { const wchar_t *fam; float size; DWRITE_FONT_WEIGHT w; } d[F_COUNT] = {
		{ L"Segoe UI Variable Display", 96, DWRITE_FONT_WEIGHT_LIGHT },
		{ L"Segoe UI Variable Display", 52, DWRITE_FONT_WEIGHT_LIGHT },
		{ L"Segoe UI Variable Display", 30, DWRITE_FONT_WEIGHT_SEMI_LIGHT },
		{ L"Segoe UI Variable Text", 24, DWRITE_FONT_WEIGHT_NORMAL },
		{ L"Segoe UI Variable Text", 19, DWRITE_FONT_WEIGHT_NORMAL },
		{ L"Segoe UI Variable Text", 16, DWRITE_FONT_WEIGHT_NORMAL },
		{ L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD },
		{ L"Segoe UI Variable Display", 58, DWRITE_FONT_WEIGHT_LIGHT },
	};
	for (int i = 0; i < F_COUNT; i++)
	{
		g_dw->CreateTextFormat(d[i].fam, nullptr, d[i].w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, d[i].size * S, L"", &g_fmt[i]);
		if (g_fmt[i]) { g_fmt[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); g_fmt[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
	}
}

static void brush(unsigned c, float a = 1.f) { g_br->SetColor(rgb(c, a)); }
static D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

static void text(const wchar_t *s, D2D1_RECT_F r, int f, unsigned c, float a = 1.f, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false)
{
	if (!g_fmt[f] || !s) return;
	g_fmt[f]->SetTextAlignment(al);
	g_fmt[f]->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
	if (wrap) g_fmt[f]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
	brush(c, a);
	g_dc->DrawText(s, (UINT32)wcslen(s), g_fmt[f], r, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
	if (wrap) g_fmt[f]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

static float text_width(const wchar_t *s, int f)
{
	IDWriteTextLayout *l = nullptr;
	g_fmt[f]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
	g_dw->CreateTextLayout(s, (UINT32)wcslen(s), g_fmt[f], 10000, 1000, &l);
	DWRITE_TEXT_METRICS m = {};
	if (l) { l->GetMetrics(&m); l->Release(); }
	return m.widthIncludingTrailingWhitespace;
}

static void fill(D2D1_RECT_F r, unsigned c, float a = 1.f, float radius = 0)
{
	brush(c, a);
	if (radius > 0) g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_br);
	else g_dc->FillRectangle(r, g_br);
}

static void stroke(D2D1_RECT_F r, unsigned c, float a = 1.f, float radius = 0, float w = 1.f)
{
	brush(c, a);
	if (radius > 0) g_dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_br, w);
	else g_dc->DrawRectangle(r, g_br, w);
}

static void hit(D2D1_RECT_F r, int screen, int idx)
{
	if (g_nhits_draw < 64) g_hits_draw[g_nhits_draw++] = { r, screen, idx };
}

static void logo(float cx, float cy, float size, float alpha = 1.f)
{
	if (!g_logo) return;
	g_dc->DrawBitmap(g_logo, D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2), alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

// menu entry: white bar on the left and graphite background when selected
static void menu_item(float x, float y, float w, float h, const wchar_t *label, bool sel, bool enabled, const wchar_t *value = nullptr)
{
	if (sel)
	{
		fill(R(x, y, w, h), GRAFITE, 1.f, 10 * S);
		fill(R(x, y + h * 0.25f, 3 * S, h * 0.5f), 0xFFFFFF);
	}
	unsigned c = !enabled ? GRIGIO : sel ? BIANCO : GRIGIO_CHIARO;
	text(label, R(x + 28 * S, y, w - 56 * S, h), F_ITEM, c);
	if (value)
	{
		wchar_t v[160];
		if (sel) swprintf(v, 160, L"‹  %s  ›", value); else wcsncpy(v, value, 159);
		text(v, R(x + 28 * S, y, w - 56 * S, h), F_BODY, sel ? BIANCO : ARGENTO, 1.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
	}
}

static void hints(const wchar_t *pad, const wchar_t *keys)
{
	text(g_pad_input ? pad : keys, R(80 * S, H - 70 * S, W - 160 * S, 30 * S), F_SMALL, GRIGIO);
}

static void footer_version()
{
	wchar_t v[64]; swprintf(v, 64, L"SaturnProject %s", SATURN_VERSION_W);
	text(v, R(W - 480 * S, H - 70 * S, 400 * S, 30 * S), F_SMALL, GRIGIO_SCURO + 0x202020, 1.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

static void toast_draw(double t)
{
	if (t > g_toast_until || !g_toast[0]) return;
	float a = (float)std::min(1.0, (g_toast_until - t) / 0.4);
	float tw = text_width(g_toast, F_BODY) + 56 * S;
	D2D1_RECT_F r = R((W - tw) / 2, 40 * S, tw, 52 * S);
	fill(r, NERO, 0.92f * a, 26 * S);
	stroke(r, GRIGIO_SCURO, a, 26 * S);
	text(g_toast, r, F_BODY, BIANCO, a, DWRITE_TEXT_ALIGNMENT_CENTER);
}

static const wchar_t *state_text(int st)
{
	switch (st)
	{
	case CS_READY: return T(L"Accesa", L"On");
	case CS_STANDBY: return T(L"In riposo", L"Rest mode");
	case CS_OFFLINE: return T(L"Non trovata in rete", L"Not found on the network");
	default: return T(L"Ricerca…", L"Searching…");
	}
}

// ---------- screens ----------
static void draw_home(double t)
{
	// background: large Saturn on the right
	logo(W * 0.68f, H * 0.47f, H * 0.95f, 0.95f);
	float x = 120 * S, w = 620 * S;
	text(L"SaturnProject", R(x, 150 * S, 1000 * S, 80 * S), F_TITLE, BIANCO);
	text(L"PlayStation 5 Remote Play", R(x + 3 * S, 222 * S, 1000 * S, 34 * S), F_BODY, GRIGIO_CHIARO);

	// console card
	D2D1_RECT_F card = R(x, 300 * S, w, 112 * S);
	fill(card, GRAFITE, 0.9f, 14 * S);
	stroke(card, GRIGIO_SCURO, 1.f, 14 * S);
	if (g_paired)
	{
		wchar_t n[96]; swprintf(n, 96, L"%hs", g_cname[0] ? g_cname : "PS5");
		text(n, R(x + 30 * S, 318 * S, w - 60 * S, 40 * S), F_H1, BIANCO);
		wchar_t st[160]; swprintf(st, 160, L"%s  ·  %hs", state_text(g_cstate), g_chost);
		float dot = 8 * S;
		D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(x + 34 * S + dot / 2, 378 * S), dot / 2, dot / 2);
		brush(g_cstate == CS_READY ? 0xFFFFFF : g_cstate == CS_STANDBY ? GRIGIO_CHIARO : GRIGIO);
		if (g_cstate == CS_OFFLINE) g_dc->DrawEllipse(e, g_br, 1.5f * S); else g_dc->FillEllipse(e, g_br);
		text(st, R(x + 54 * S, 362 * S, w - 80 * S, 32 * S), F_SMALL, GRIGIO_CHIARO);
	}
	else
	{
		text(T(L"Nessuna PS5 associata", L"No PS5 paired"), R(x + 30 * S, 318 * S, w - 60 * S, 40 * S), F_H1, ARGENTO);
		text(T(L"Scegli «Associa una PS5» per iniziare.", L"Choose “Pair a PS5” to begin."), R(x + 30 * S, 362 * S, w - 60 * S, 32 * S), F_SMALL, GRIGIO_CHIARO);
	}

	const wchar_t *labels[H_COUNT] = {
		T(L"Gioca", L"Play"), T(L"Impostazioni", L"Settings"),
		g_paired ? T(L"Associa un'altra PS5", L"Pair another PS5") : T(L"Associa una PS5", L"Pair a PS5"), T(L"Esci", L"Quit") };
	float y = 460 * S, ih = 66 * S;
	for (int i = 0; i < H_COUNT; i++)
	{
		bool en = i != H_PLAY || g_paired;
		menu_item(x, y, w, ih, labels[i], g_home_sel == i, en);
		hit(R(x, y, w, ih), SCR_HOME, i);
		y += ih + 6 * S;
	}
	if (t < g_err_until && g_err[0])
	{
		D2D1_RECT_F r = R(x, y + 24 * S, w, 70 * S);
		fill(r, GRIGIO_SCURO, 0.9f, 10 * S);
		text(g_err, R(x + 24 * S, y + 30 * S, w - 48 * S, 60 * S), F_SMALL, BIANCO, 1.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
	}
	const char *pn = pad_name();
	wchar_t ph[200];
	if (pn[0]) swprintf(ph, 200, T(L"✕ Seleziona     ○ Indietro          Controller: %hs", L"✕ Select     ○ Back          Controller: %hs"), pn);
	else wcscpy(ph, T(L"Invio Seleziona     Esc Esci          Nessun controller collegato", L"Enter Select     Esc Quit          No controller connected"));
	hints(ph, pn[0] ? T(L"Invio Seleziona     Esc Esci", L"Enter Select     Esc Quit") : ph);
	footer_version();
}

static void draw_field(float x, float y, float w, float h, const wchar_t *label, const wchar_t *value, const wchar_t *placeholder, bool sel, bool caret, double t)
{
	menu_item(x, y, w, h, label, sel, true);
	D2D1_RECT_F box = R(x + w * 0.42f, y + 9 * S, w * 0.58f - 20 * S, h - 18 * S);
	fill(box, NERO, 1.f, 8 * S);
	stroke(box, sel ? GRIGIO_CHIARO : GRIGIO_SCURO, 1.f, 8 * S);
	bool empty = !value[0];
	wchar_t v[96]; swprintf(v, 96, L"%s%s", empty ? L"" : value, caret && sel && fmod(t, 1.0) < 0.55 ? L"│" : L"");
	if (empty && !(caret && sel)) text(placeholder, R(box.left + 16 * S, box.top, box.right - box.left - 32 * S, box.bottom - box.top), F_BODY, GRIGIO);
	else text(v, R(box.left + 16 * S, box.top, box.right - box.left - 32 * S, box.bottom - box.top), F_BODY, BIANCO);
}

static void draw_pair(double t)
{
	logo(W * 0.80f, H * 0.5f, H * 0.62f, 0.5f);
	float x = 120 * S, w = 1000 * S;
	text(T(L"Associa una PS5", L"Pair a PS5"), R(x, 90 * S, w, 70 * S), F_TITLE, BIANCO);
	text(T(L"Sulla PS5: Impostazioni › Sistema › Riproduzione remota › Collega dispositivo. Comparir\u00E0 un codice di 8 cifre.",
		L"On the PS5: Settings › System › Remote Play › Pair Device. An 8-digit code will appear."),
		R(x, 165 * S, w, 60 * S), F_BODY, GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
	float y = 250 * S, ih = 66 * S;
	for (int i = 0; i < P_COUNT; i++)
	{
		if (!pair_visible(i)) continue;
		bool sel = g_pair_sel == i;
		switch (i)
		{
		case P_CONSOLE:
		{
			wchar_t v[160];
			if (g_scanning && !g_nfound) wcscpy(v, T(L"Ricerca in rete…", L"Searching the network…"));
			else if (pair_manual()) wcscpy(v, T(L"Inserisci l'indirizzo IP", L"Enter the IP address"));
			else swprintf(v, 160, L"%hs  ·  %hs  ·  %s", g_found[g_found_sel].name, g_found[g_found_sel].host, state_text(g_found[g_found_sel].state));
			menu_item(x, y, w, ih, T(L"Console", L"Console"), sel, true, v);
			break;
		}
		case P_IP: draw_field(x, y, w, ih, T(L"Indirizzo IP", L"IP address"), g_ip, L"192.168.1.x", sel, true, t); break;
		case P_ACCOUNT: draw_field(x, y, w, ih, T(L"Account ID PSN", L"PSN Account ID"), g_account, T(L"es. AbCdEfGhIjk=", L"e.g. AbCdEfGhIjk="), sel, true, t); break;
		case P_PSN:
			menu_item(x, y, w, ih, g_psn_busy ? T(L"Lettura dell'account…", L"Reading the account…") : g_psn_wait ? T(L"In attesa: copia l'indirizzo della pagina finale", L"Waiting: copy the final page address")
				: T(L"Trova l'Account ID con l'accesso PSN", L"Get the Account ID by signing in to PSN"), sel, true);
			break;
		case P_PIN: draw_field(x, y, w, ih, T(L"Codice della PS5", L"PS5 code"), g_pin, L"12345678", sel, true, t); break;
		case P_GO: menu_item(x, y, w, ih, g_pairing ? T(L"Associazione in corso…", L"Pairing…") : T(L"Associa", L"Pair"), sel, !g_pairing); break;
		}
		hit(R(x, y, w, ih), SCR_PAIR, i);
		y += ih + 6 * S;
	}
	const wchar_t *help = g_pair_sel == P_PSN ? T(L"Si apre il browser: accedi a PlayStation Network. Alla fine la pagina resta bianca o dice errore: copia il suo indirizzo (Ctrl+L, Ctrl+C) e SaturnProject lo legge da solo.",
		L"Your browser opens: sign in to PlayStation Network. The final page is blank or shows an error: copy its address (Ctrl+L, Ctrl+C) and SaturnProject reads it automatically.")
		: g_pair_sel == P_ACCOUNT ? T(L"Ctrl+V per incollare. Se non lo conosci usa la voce sotto.", L"Ctrl+V to paste. If you don't know it, use the option below.")
		: g_pair_sel == P_CONSOLE ? T(L"← → per scegliere, ✕ / Invio per cercare di nuovo.", L"← → to choose, ✕ / Enter to search again.") : L"";
	text(help, R(x, y + 14 * S, w, 70 * S), F_SMALL, GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
	if (g_pair_msg[0]) text(g_pair_msg, R(x, y + 90 * S, w, 60 * S), F_BODY, BIANCO, 1.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
	hints(T(L"✕ Seleziona     ○ Indietro     (per scrivere serve la tastiera)", L"✕ Select     ○ Back     (typing needs a keyboard)"),
		T(L"Invio Seleziona     Esc Indietro     Ctrl+V Incolla", L"Enter Select     Esc Back     Ctrl+V Paste"));
	footer_version();
}

static void draw_connect(double t)
{
	float cx = W / 2, cy = H * 0.44f, size = 380 * S;
	logo(cx, cy, size, 1.f);
	// arc spinning around the logo
	if (g_arc)
	{
		float r = size * 0.62f;
		D2D1_MATRIX_3X2_F old; g_dc->GetTransform(&old);
		g_dc->SetTransform(D2D1::Matrix3x2F::Scale(r, r) * D2D1::Matrix3x2F::Rotation((float)fmod(t * 200.0, 360.0)) * D2D1::Matrix3x2F::Translation(cx, cy));
		brush(GRIGIO_CHIARO, 0.9f);
		g_dc->DrawGeometry(g_arc, g_br, 2.0f * S / r);
		g_dc->SetTransform(old);
		brush(GRIGIO_SCURO);
		g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, 1.f * S);
	}
	text(g_status, R(0, cy + size * 0.75f, W, 50 * S), F_H1, BIANCO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
	wchar_t sub[160]; swprintf(sub, 160, L"%hs  ·  %hs", g_cname[0] ? g_cname : "PS5", g_chost);
	text(sub, R(0, cy + size * 0.75f + 52 * S, W, 34 * S), F_BODY, GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
	hints(T(L"○ Annulla", L"○ Cancel"), T(L"Esc Annulla", L"Esc Cancel"));
}

// pacing of the shown images: fps of the last second, 1% low (worst 1% of the intervals in 10 s), graph.
// Updated 4 times per second: recomputed on every image the numbers change constantly and the graph scrolls in jumps.
static float g_ft[1200];
static int g_ftn;
static double g_fps, g_low;
static void update_pacing()
{
	static double last_upd;
	double now = now_s();
	if (now - last_upd <= 0.25) return;
	last_upd = now;
	g_ftn = video_frame_times(g_ft, 1200, 10.0);
	double sum = 0; int k = 0;
	for (int i = g_ftn - 1; i >= 0 && sum < 1000.0; i--) { sum += g_ft[i]; k++; }
	g_fps = sum > 0 ? k * 1000.0 / sum : 0;
	g_low = 0;
	if (g_ftn > 20)
	{
		static float srt[1200];
		memcpy(srt, g_ft, g_ftn * sizeof(float));
		std::sort(srt, srt + g_ftn);
		int m = std::max(1, g_ftn / 100);
		double s2 = 0; for (int i = g_ftn - m; i < g_ftn; i++) s2 += srt[i];
		g_low = 1000.0 / (s2 / m);
	}
}

// interval graph (last 2 s) with the display period line
static void pacing_graph(D2D1_RECT_F r)
{
	float px = r.left, pw = r.right - r.left, gy = r.top, gh = r.bottom - r.top;
	fill(r, GRAFITE, 1.f, 6 * S);
	double period = g_have_stats && g_stats.refresh > 0 ? 1000.0 / g_stats.refresh : 8.33;
	double vmax = period * 3;
	float ry = gy + gh - (float)(period / vmax) * gh;
	brush(GRIGIO, 0.8f); g_dc->DrawLine(D2D1::Point2F(px, ry), D2D1::Point2F(px + pw, ry), g_br, 1.f * S);
	int show = std::min(g_ftn, 240);
	if (show < 2) return;
	brush(ARGENTO);
	float dx = pw / 239.f;
	for (int i = 1; i < show; i++)
	{
		float a = (float)std::min((double)g_ft[g_ftn - show + i - 1], vmax), c = (float)std::min((double)g_ft[g_ftn - show + i], vmax);
		float x0 = px + pw - (show - i) * dx, x1 = x0 + dx;
		g_dc->DrawLine(D2D1::Point2F(x0, gy + gh - a / (float)vmax * gh), D2D1::Point2F(x1, gy + gh - c / (float)vmax * gh), g_br, 1.4f * S);
	}
}

// statistics panel over the game (F2): small and transparent in the top right, so the game stays visible
static void draw_stats()
{
	update_pacing();
	float w = 300 * S, x = W - 24 * S - w, y = 24 * S, h = g_have_stats ? 132 * S : 92 * S;
	fill(R(x, y, w, h), 0x000000, 0.42f, 10 * S);
	float px = x + 14 * S, pw = w - 28 * S;
	wchar_t b[160];
	swprintf(b, 160, L"%.0f", g_fps);
	float nw = text_width(b, F_H1);
	text(b, R(px, y + 6 * S, pw, 40 * S), F_H1, BIANCO, 0.92f);
	text(L"fps", R(px + nw + 4 * S, y + 14 * S, 50 * S, 28 * S), F_SMALL, GRIGIO_CHIARO, 0.9f);
	swprintf(b, 160, L"1%% low %.0f", g_low);
	text(b, R(px, y + 12 * S, pw, 28 * S), F_SMALL, ARGENTO, 0.9f, DWRITE_TEXT_ALIGNMENT_TRAILING);
	D2D1_RECT_F gr = R(px, y + 52 * S, pw, 30 * S);
	pacing_graph(gr);
	if (g_have_stats)
	{
		const UiStats &s2 = g_stats;
		if (s2.fg) swprintf(b, 160, L"%hs  ·  %.0f/s  ·  %.1f ms", s2.fg_engine, s2.fps_generated, s2.fg_ms);
		else wcscpy(b, T(L"Generazione spenta", L"Generation off"));
		text(b, R(px, y + 88 * S, pw, 20 * S), F_TINY, GRIGIO_CHIARO, 0.9f);
		swprintf(b, 160, L"%dp%.0f  ·  %s  ·  %.0f Mbit/s  ·  RTT %.0f ms", s2.src_h, s2.fps_source, s2.h265 ? L"H.265" : L"H.264", s2.mbit, s2.rtt_ms);
		text(b, R(px, y + 108 * S, pw, 20 * S), F_TINY, GRIGIO_CHIARO, 0.9f);
	}
}

// ---------- tabbed panel: in-game menu (game scaled down, statistics below) and settings ----------
static int g_tab = T_PICTURE;
static bool g_last_menu; static double g_menu_t = -10;

static bool tab_visible(int t, bool in_game) { return in_game || t != T_GAME; }
static int first_item(int tab) { for (int i = 0; i < N_ITEMS; i++) if (g_items[i].section == tab) return i; return 0; }
static void set_tab(int t) { g_tab = t; g_set_sel = first_item(t); g_confirm = -1; }
static void tab_step(int dir, bool in_game) { int t = g_tab; do t = (t + dir + T_COUNT) % T_COUNT; while (!tab_visible(t, in_game)); set_tab(t); }
static void item_step(int dir) { int i = g_set_sel; do i = (i + dir + N_ITEMS) % N_ITEMS; while (g_items[i].section != g_tab); g_set_sel = i; g_confirm = -1; }

// panel layout for a w x h screen (also used by the video thread to scale the game down)
struct Layout { float s, m, pw, px, vx, vy, vw, vh, by, bh; };
static Layout layout(float w, float h)
{
	Layout L;
	L.s = std::min(w / 1920.f, h / 1080.f);
	L.m = 16 * L.s; L.pw = 470 * L.s; L.px = w - L.m - L.pw;
	L.vx = L.m; L.vy = L.m; L.vw = L.px - 2 * L.m; L.vh = L.vw * 9 / 16;
	float maxh = h - 3 * L.m - 230 * L.s;
	if (L.vh > maxh) L.vh = maxh;
	L.by = L.vy + L.vh + L.m; L.bh = h - L.m - L.by;
	return L;
}

// 0 = full screen game, 1 = scaled down (menu open); 0.2 s animation. With g_lock held.
static float menu_k(double t)
{
	if (g_menu != g_last_menu) { g_last_menu = g_menu; g_menu_t = t; }
	float a = (float)std::min(1.0, (t - g_menu_t) / 0.2);
	a = 1 - (1 - a) * (1 - a) * (1 - a); // ease out
	return g_menu ? a : 1 - a;
}

bool ui_video_rect(int out_w, int out_h, RECT *r)
{
	EnterCriticalSection(&g_lock);
	float k = g_scr == SCR_GAME ? menu_k(now_s()) : 0;
	LeaveCriticalSection(&g_lock);
	if (k <= 0) return false;
	Layout L = layout((float)out_w, (float)out_h);
	r->left = (LONG)(L.vx * k); r->top = (LONG)(L.vy * k);
	r->right = (LONG)(out_w + (L.vx + L.vw - out_w) * k); r->bottom = (LONG)(out_h + (L.vy + L.vh - out_h) * k);
	return true;
}

static void card(D2D1_RECT_F r, const wchar_t *title)
{
	fill(r, 0x101010, 0.97f, 12 * S);
	stroke(r, GRIGIO_SCURO, 1.f, 12 * S);
	if (title) text(title, R(r.left + 20 * S, r.top + 12 * S, r.right - r.left - 40 * S, 22 * S), F_TINY, GRIGIO);
}

// pill-style switch
static void switch_pill(float x, float y, bool on, float a)
{
	D2D1_RECT_F r = R(x, y, 46 * S, 26 * S);
	fill(r, on ? BIANCO : GRIGIO_SCURO, a, 13 * S);
	float kx = on ? x + 33 * S : x + 13 * S;
	brush(on ? NERO : GRIGIO_CHIARO, a);
	g_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, y + 13 * S), 9 * S, 9 * S), g_br);
}

// progress ring (0..1) with the value in the center and a caption below
static void ring(float cx, float cy, float r, float v, const wchar_t *val, const wchar_t *cap)
{
	brush(GRIGIO_SCURO); g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, 4 * S);
	v = std::max(0.f, std::min(0.999f, v));
	if (v > 0.002f)
	{
		ID2D1PathGeometry *g = nullptr;
		if (SUCCEEDED(g_f->CreatePathGeometry(&g)))
		{
			ID2D1GeometrySink *sk = nullptr; g->Open(&sk);
			float a0 = -3.14159265f / 2, a1 = a0 + v * 2 * 3.14159265f;
			sk->BeginFigure(D2D1::Point2F(cx + r * cosf(a0), cy + r * sinf(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
			sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * cosf(a1), cy + r * sinf(a1)), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, v > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
			sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close(); sk->Release();
			brush(BIANCO); g_dc->DrawGeometry(g, g_br, 4 * S);
			g->Release();
		}
	}
	text(val, R(cx - r, cy - 12 * S, 2 * r, 24 * S), F_TINY, BIANCO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
	text(cap, R(cx - r - 20 * S, cy + r + 6 * S, 2 * r + 40 * S, 20 * S), F_TINY, GRIGIO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// the three cards under the game: signal, timings, frame rate and pacing
static void draw_stat_cards(const Layout &L, float a)
{
	update_pacing();
	float gap = L.m, cw = (L.vw - 2 * gap) / 3, y = L.by, h = L.bh;
	wchar_t b[160];
	const UiStats &s = g_stats;
	// --- signal ---
	D2D1_RECT_F c1 = R(L.vx, y, cw, h);
	card(c1, T(L"SEGNALE", L"SIGNAL"));
	float rx = c1.left + 20 * S, rw = cw - 40 * S, ry = y + 44 * S, lh = (h - 60 * S) / 5;
	auto row = [&](const wchar_t *k2, const wchar_t *v) {
		text(k2, R(rx, ry, rw, lh), F_SMALL, GRIGIO_CHIARO);
		text(v, R(rx, ry, rw, lh), F_SMALL, ARGENTO, 1.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
		ry += lh;
	};
	if (g_have_stats)
	{
		swprintf(b, 160, L"%d×%d  ·  %.0f fps", s.src_w, s.src_h, s.fps_source); row(T(L"Ingresso", L"Input"), b);
		swprintf(b, 160, L"%d×%d  ·  %d Hz", s.out_w, s.out_h, s.refresh); row(T(L"Uscita", L"Output"), b);
		swprintf(b, 160, L"%s  ·  %.1f Mbit/s", s.h265 ? L"H.265" : L"H.264", s.mbit); row(T(L"Codifica", L"Codec"), b);
		if (s.fg) swprintf(b, 160, L"%hs  ·  %.0f/s", s.fg_engine, s.fps_generated); else wcscpy(b, T(L"spenta", L"off"));
		row(T(L"Generazione", L"Generation"), b);
		row(T(L"Ingrandimento", L"Upscaling"), s.vsr ? L"RTX Video Super Resolution" : T(L"standard", L"standard"));
	}
	// --- timings ---
	D2D1_RECT_F c2 = R(L.vx + cw + gap, y, cw, h);
	card(c2, T(L"TEMPI  ·  MEDIA DELL'ULTIMO SECONDO", L"TIMINGS  ·  LAST SECOND AVERAGE"));
	rx = c2.left + 20 * S; ry = y + 44 * S; lh = (h - 60 * S) / 4;
	double period = s.refresh > 0 ? 1000.0 / s.refresh : 8.33;
	auto bar = [&](const wchar_t *k2, double v, double full, const wchar_t *unit) {
		text(k2, R(rx, ry, rw * 0.36f, lh), F_SMALL, GRIGIO_CHIARO);
		float bx = rx + rw * 0.36f, bw = rw * 0.40f, bh2 = 4 * S, byy = ry + lh / 2 - bh2 / 2;
		fill(R(bx, byy, bw, bh2), GRIGIO_SCURO, 1.f, 2 * S);
		fill(R(bx, byy, bw * (float)std::min(1.0, v / full), bh2), BIANCO, 1.f, 2 * S);
		swprintf(b, 160, L"%.1f %s", v, unit);
		text(b, R(rx, ry, rw, lh), F_SMALL, ARGENTO, 1.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
		ry += lh;
	};
	if (g_have_stats)
	{
		bar(T(L"Decodifica", L"Decode"), s.decode_ms, period, L"ms");
		bar(T(L"Generazione", L"Generation"), s.fg ? s.fg_ms : 0, period, L"ms");
		bar(T(L"Rete (RTT)", L"Network (RTT)"), s.rtt_ms, 20, L"ms");
		bar(T(L"Ritardo audio", L"Audio delay"), s.av_auto + g_set.audio_delay, 100, L"ms");
	}
	// --- frame rate and pacing ---
	D2D1_RECT_F c3 = R(L.vx + 2 * (cw + gap), y, cw, h);
	card(c3, T(L"FREQUENZA E RITMO", L"FRAME RATE AND PACING"));
	rx = c3.left + 20 * S;
	swprintf(b, 160, L"%.1f", g_fps);
	float nw = text_width(b, F_H1);
	text(b, R(rx, y + 40 * S, rw, 44 * S), F_H1, BIANCO);
	text(L"fps", R(rx + nw + 6 * S, y + 50 * S, 60 * S, 30 * S), F_SMALL, GRIGIO_CHIARO);
	swprintf(b, 160, L"1%% low %.1f", g_low);
	text(b, R(rx, y + 86 * S, rw * 0.5f, 24 * S), F_SMALL, ARGENTO);
	float gx = rx, gw2 = rw * 0.48f, gy = y + 118 * S, gh = h - 136 * S;
	if (gh > 10 * S) pacing_graph(R(gx, gy, gw2, gh));
	// rings: steadiness (1% low / fps), generated images, new images at every display refresh
	float rr = std::min(30 * S, (h - 90 * S) / 2), ry0 = y + 44 * S + rr + 10 * S;
	float rgx = rx + rw * 0.56f, rstep = (rw * 0.44f) / 3;
	wchar_t v1[32], v2[32], v3[32];
	double steady = g_fps > 0 ? std::min(1.0, g_low / g_fps) : 0;
	double gen = g_have_stats && s.fps_shown > 0 ? s.fps_generated / s.fps_shown : 0;
	double fresh = g_have_stats && s.refresh > 0 ? std::max(0.0, 1.0 - s.repeats / s.refresh) : 0;
	swprintf(v1, 32, L"%.0f%%", steady * 100); swprintf(v2, 32, L"%.0f%%", gen * 100); swprintf(v3, 32, L"%.0f%%", fresh * 100);
	ring(rgx + rstep * 0.5f, ry0, rr, (float)steady, v1, T(L"Regolarità", L"Steadiness"));
	ring(rgx + rstep * 1.5f, ry0, rr, (float)gen, v2, T(L"Generate", L"Generated"));
	ring(rgx + rstep * 2.5f, ry0, rr, (float)fresh, v3, T(L"Nuove", L"Fresh"));
	(void)a;
}

// ---------- Buttons tab: 3 named profiles and a drawn DualSense ----------
// items: 0-2 profiles, 3-14 pad buttons (order Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Create), 15 Rename, 16 Reset
enum { KF_BTN = 3, KF_RENAME = 15, KF_RESET = 16, KF_COUNT = 17 };
static int g_kfocus = KF_BTN;
static bool g_kchoose, g_krename;
static D2D1_POINT_2F g_kpos[KF_COUNT];
static const wchar_t *BTN_SYM[12] = { L"✕", L"○", L"□", L"△", L"L1", L"R1", L"L2", L"R2", L"L3", L"R3", L"Opt", L"Crt" };
static const wchar_t *BTN_NAME[12] = { L"✕", L"○", L"□", L"△", L"L1", L"R1", L"L2", L"R2", L"L3", L"R3", L"Options", L"Create" };
static int *kmap() { return g_set.kp_map[g_set.key_profile]; }

// "spatial" navigation: the closest item in the chosen direction
static void keys_move(int dx, int dy)
{
	D2D1_POINT_2F c = g_kpos[g_kfocus];
	int best = -1; float bs = 1e9f;
	for (int i = 0; i < KF_COUNT; i++)
	{
		if (i == g_kfocus) continue;
		float ex = g_kpos[i].x - c.x, ey = g_kpos[i].y - c.y;
		float along = ex * dx + ey * dy, side = fabsf(ex * dy) + fabsf(ey * dx);
		if (along <= 4) continue;
		float sc = along + side * 2.2f;
		if (sc < bs) { bs = sc; best = i; }
	}
	if (best >= 0) g_kfocus = best;
}

static void keys_activate(bool click)
{
	if (g_kfocus < 3) { g_set.key_profile = g_kfocus; changed(); }
	else if (g_kfocus < KF_RENAME)
	{
		(void)click;
		g_kchoose = true; // now the button to send is pressed on the pad (or arrows on the keyboard)
	}
	else if (g_kfocus == KF_RENAME) g_krename = true;
	else
	{
		for (int k = 0; k < 12; k++) kmap()[k] = k;
		changed();
		swprintf(g_toast, 160, T(L"%s: tasti ripristinati", L"%s: buttons reset"), g_set.kp_name[g_set.key_profile]); g_toast_until = now_s() + 2;
	}
}

// Buttons tab commands; false = not handled here (tabs, close)
static bool keys_cmd(UiPad b)
{
	if (g_krename) { if (b == UP_OK || b == UP_BACK) { g_krename = false; if (!g_set.kp_name[g_set.key_profile][0]) swprintf(g_set.kp_name[g_set.key_profile], 24, L"Profile %d", g_set.key_profile + 1); changed(); } return true; }
	if (g_kchoose)
	{
		int bi = g_kfocus - KF_BTN;
		if (b == UP_LEFT || b == UP_RIGHT) { kmap()[bi] = (kmap()[bi] + (b == UP_LEFT ? 11 : 1)) % 12; changed(); return true; }
		if (b == UP_OK || b == UP_BACK) { g_kchoose = false; return true; }
		if (b == UP_UP || b == UP_DOWN) g_kchoose = false; // and moves
		else return false;
	}
	switch (b)
	{
	case UP_UP: keys_move(0, -1); return true;
	case UP_DOWN: keys_move(0, 1); return true;
	case UP_LEFT: keys_move(-1, 0); return true;
	case UP_RIGHT: keys_move(1, 0); return true;
	case UP_OK: keys_activate(false); return true;
	default: return false;
	}
}

static void draw_keys(float x, float y, float w, bool in_game, double t)
{
	int scr = in_game ? SCR_GAME : SCR_SETTINGS;
	// profiles
	float pw = (w - 16 * S) / 3, ph = 56 * S;
	for (int i = 0; i < 3; i++)
	{
		D2D1_RECT_F r = R(x + i * (pw + 8 * S), y, pw, ph);
		bool act = g_set.key_profile == i, foc = g_kfocus == i;
		fill(r, foc ? GRIGIO_SCURO : NERO, 1.f, 10 * S);
		stroke(r, act ? BIANCO : GRIGIO_SCURO, 1.f, 10 * S, act ? 2.f * S : 1.f * S);
		wchar_t nm[40]; swprintf(nm, 40, L"%s%s", g_set.kp_name[i], g_krename && act && fmod(t, 1.0) < 0.55 ? L"│" : L"");
		text(nm, R(r.left + 12 * S, r.top + 6 * S, pw - 24 * S, 26 * S), F_SMALL, act ? BIANCO : ARGENTO);
		text(act ? T(L"attivo", L"active") : T(L"scegli", L"select"), R(r.left + 12 * S, r.top + 30 * S, pw - 24 * S, 20 * S), F_TINY, act ? GRIGIO_CHIARO : GRIGIO);
		g_kpos[i] = D2D1::Point2F(r.left + pw / 2, r.top + ph / 2);
		hit(r, scr, 2000 + i);
	}
	y += ph + 18 * S;
	// DualSense (drawing in 460 x 250 units)
	float u = w / 460.f;
	auto P = [&](float a, float b2) { return D2D1::Point2F(x + a * u, y + b2 * u); };
	brush(0x1A1A1A);
	g_dc->FillEllipse(D2D1::Ellipse(P(118, 200), 78 * u, 70 * u), g_br);
	g_dc->FillEllipse(D2D1::Ellipse(P(342, 200), 78 * u, 70 * u), g_br);
	g_dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 40 * u, y + 52 * u, x + 420 * u, y + 200 * u), 60 * u, 60 * u), g_br);
	brush(GRIGIO_SCURO);
	g_dc->DrawEllipse(D2D1::Ellipse(P(118, 200), 78 * u, 70 * u), g_br, 1.f * S);
	g_dc->DrawEllipse(D2D1::Ellipse(P(342, 200), 78 * u, 70 * u), g_br, 1.f * S);
	brush(0x1A1A1A);
	g_dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 41 * u, y + 53 * u, x + 419 * u, y + 199 * u), 60 * u, 60 * u), g_br);
	fill(D2D1::RectF(x + 160 * u, y + 60 * u, x + 300 * u, y + 128 * u), 0x111111, 1.f, 10 * u);  // touchpad
	stroke(D2D1::RectF(x + 160 * u, y + 60 * u, x + 300 * u, y + 128 * u), GRIGIO_SCURO, 1.f, 10 * u);
	// d-pad (not remappable)
	brush(0x2A2A2A);
	g_dc->FillRectangle(D2D1::RectF(x + 104 * u, y + 92 * u, x + 126 * u, y + 148 * u), g_br);
	g_dc->FillRectangle(D2D1::RectF(x + 87 * u, y + 109 * u, x + 143 * u, y + 131 * u), g_br);
	struct B { float cx, cy, rw, rh; bool round; } bp[12] = {
		{ 345, 150, 17, 17, true }, { 380, 118, 17, 17, true }, { 310, 118, 17, 17, true }, { 345, 86, 17, 17, true },
		{ 110, 36, 34, 10, false }, { 350, 36, 34, 10, false }, { 110, 12, 34, 12, false }, { 350, 12, 34, 12, false },
		{ 178, 172, 24, 24, true }, { 282, 172, 24, 24, true }, { 318, 70, 13, 9, false }, { 142, 70, 13, 9, false } };
	for (int i = 0; i < 12; i++)
	{
		const B &b = bp[i];
		D2D1_RECT_F r = D2D1::RectF(x + (b.cx - b.rw) * u, y + (b.cy - b.rh) * u, x + (b.cx + b.rw) * u, y + (b.cy + b.rh) * u);
		bool foc = g_kfocus == KF_BTN + i, moved = kmap()[i] != i;
		float rad = b.round ? b.rw * u : 6 * u;
		fill(r, moved ? BIANCO : 0x2A2A2A, 1.f, rad);
		if (foc) stroke(D2D1::RectF(r.left - 4 * u, r.top - 4 * u, r.right + 4 * u, r.bottom + 4 * u), BIANCO, 1.f, rad + 4 * u, (g_kchoose ? 3.f : 2.f) * S);
		// inside: the button that really reaches the PS5 (black if different from its own)
		text(BTN_SYM[moved ? kmap()[i] : i], r, i >= 10 ? F_TINY : F_SMALL, moved ? NERO : ARGENTO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
		g_kpos[KF_BTN + i] = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
		hit(D2D1::RectF(r.left - 6 * u, r.top - 6 * u, r.right + 6 * u, r.bottom + 6 * u), scr, 2000 + KF_BTN + i);
	}
	y += 278 * u;
	// status line of the chosen button
	wchar_t line[160] = {};
	if (g_kfocus >= KF_BTN && g_kfocus < KF_RENAME)
	{
		int bi = g_kfocus - KF_BTN;
		if (g_kchoose) swprintf(line, 160, T(L"Premi sul pad il tasto che %s deve inviare", L"Press on the pad the button %s should send"), BTN_NAME[bi]);
		else swprintf(line, 160, T(L"%s invia  %s", L"%s sends  %s"), BTN_NAME[bi], BTN_NAME[kmap()[bi]]);
	}
	text(line, R(x, y, w, 30 * S), F_BODY, BIANCO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
	y += 44 * S;
	// actions
	const wchar_t *lab[2] = { g_krename ? T(L"Scrivi il nome, Invio per finire", L"Type the name, Enter to finish") : T(L"Rinomina profilo", L"Rename profile"), T(L"Ripristina profilo", L"Reset profile") };
	float bw = (w - 8 * S) / 2;
	for (int i = 0; i < 2; i++)
	{
		D2D1_RECT_F r = R(x + i * (bw + 8 * S), y, bw, 46 * S);
		bool foc = g_kfocus == KF_RENAME + i;
		fill(r, foc ? GRIGIO_SCURO : NERO, 1.f, 10 * S);
		stroke(r, foc ? GRIGIO_CHIARO : GRIGIO_SCURO, 1.f, 10 * S);
		text(lab[i], r, F_SMALL, foc ? BIANCO : ARGENTO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
		g_kpos[KF_RENAME + i] = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
		hit(r, scr, 2000 + KF_RENAME + i);
	}
}

// the right panel with the tabs
static void draw_panel(const Layout &L, bool in_game, double t)
{
	D2D1_RECT_F pr = R(L.px, L.m, L.pw, H - 2 * L.m);
	card(pr, nullptr);
	float x = L.px + 20 * S, w = L.pw - 40 * S, y = L.m + 18 * S;
	// header
	logo(x + 26 * S, y + 26 * S, 52 * S);
	text(L"SaturnProject", R(x + 64 * S, y + 2 * S, w - 64 * S, 28 * S), F_ITEM, BIANCO);
	wchar_t sub[160]; swprintf(sub, 160, L"%hs  \u00B7  %s", g_cname[0] ? g_cname : "PS5", in_game ? T(L"in gioco", L"playing") : state_text(g_cstate));
	{
		bool chg = false; int bat = pad_battery(&chg);
		if (bat >= 0) { size_t n = wcslen(sub); swprintf(sub + n, 160 - n, L"  \u00B7  %s %d%%%s", T(L"pad", L"pad"), bat, chg ? T(L" in carica", L" charging") : L""); }
	}
	text(sub, R(x + 64 * S, y + 30 * S, w - 64 * S, 22 * S), F_SMALL, GRIGIO_CHIARO);
	y += 72 * S;
	// tabs
	int nt = 0; for (int t2 = 0; t2 < T_COUNT; t2++) if (tab_visible(t2, in_game)) nt++;
	float tw = w / nt, th = 34 * S;
	fill(R(x, y, w, th), NERO, 1.f, 8 * S);
	float tx = x;
	for (int t2 = 0; t2 < T_COUNT; t2++)
	{
		if (!tab_visible(t2, in_game)) continue;
		bool sel = g_tab == t2;
		if (sel) fill(R(tx + 2 * S, y + 2 * S, tw - 4 * S, th - 4 * S), GRIGIO_SCURO, 1.f, 7 * S);
		text(T(TABS_IT[t2], TABS_EN[t2]), R(tx, y, tw, th), F_TINY, sel ? BIANCO : GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_CENTER);
		hit(R(tx, y, tw, th), in_game ? SCR_GAME : SCR_SETTINGS, 1000 + t2);
		tx += tw;
	}
	y += th + 16 * S;
	if (g_tab == T_KEYS)
	{
		draw_keys(x, y, w, in_game, t);
		float dy2 = H - L.m - 150 * S;
		fill(R(x, dy2, w, 1 * S), GRIGIO_SCURO);
		text(T(L"Clic su un tasto del disegno, poi premi sul pad il tasto che deve inviare alla PS5 (Esc annulla). In bianco i tasti cambiati. Il pad resta al gioco tranne in quel momento.",
			L"Click a button on the drawing, then press on the pad the button it should send to the PS5 (Esc cancels). Changed buttons are white. The pad stays with the game except at that moment."),
			R(x, dy2 + 14 * S, w, 80 * S), F_SMALL, GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
		text(g_pad_input ? T(L"L1 R1 Schede   \u2715 Scegli   \u25CB Indietro", L"L1 R1 Tabs   \u2715 Select   \u25CB Back") : T(L"PagSu PagGi\u00F9 Schede   Invio Scegli   Esc Indietro", L"PgUp PgDn Tabs   Enter Select   Esc Back"),
			R(x, H - L.m - 46 * S, w, 26 * S), F_TINY, GRIGIO);
		return;
	}
	// tab entries (they scroll if they do not fit: the selection always stays visible)
	float ih = 64 * S, step = ih + 4 * S;
	float ly0 = y, ly1 = H - L.m - 160 * S;
	int nit = 0, selpos = 0;
	for (int i = 0; i < N_ITEMS; i++) if (g_items[i].section == g_tab) { if (i == g_set_sel) selpos = nit; nit++; }
	float total = nit * step, avail = ly1 - ly0, scroll = 0;
	if (total > avail) { scroll = selpos * step + ih / 2 - avail / 2; scroll = std::max(0.f, std::min(scroll, total - avail)); }
	y -= scroll;
	g_dc->PushAxisAlignedClip(D2D1::RectF(x - 2 * S, ly0, x + w + 2 * S, ly1), D2D1_ANTIALIAS_MODE_ALIASED);
	for (int i = 0; i < N_ITEMS; i++)
	{
		const SItem &it = g_items[i];
		if (it.section != g_tab) continue;
		if (y + ih < ly0 || y > ly1) { y += step; continue; }
		bool sel = g_set_sel == i;
		D2D1_RECT_F r = R(x, y, w, ih);
		if (sel) { fill(r, GRIGIO_SCURO, 0.7f, 10 * S); fill(R(x, y + ih * 0.28f, 3 * S, ih * 0.44f), 0xFFFFFF); }
		float lx = x + 18 * S, lw = w - 36 * S;
		text(T(it.it, it.en), R(lx, y, lw * 0.62f, ih), F_BODY, sel ? BIANCO : ARGENTO);
		wchar_t v[160] = {};
		switch (it.kind)
		{
		case K_BOOL: switch_pill(x + w - 18 * S - 46 * S, y + (ih - 26 * S) / 2, *it.b, 1.f); break;
		case K_CHOICE: case K_RANGE:
		{
			wchar_t raw[128]; item_value(it, raw, 128);
			if (sel) swprintf(v, 160, L"‹  %s  ›", raw); else wcscpy(v, raw);
			text(v, R(lx + lw * 0.40f, y, lw * 0.60f, ih), F_SMALL, sel ? BIANCO : GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
			break;
		}
		case K_ACTION:
			text(g_confirm == i ? T(L"Premi di nuovo", L"Press again") : L"›", R(lx, y, lw, ih), F_SMALL, sel ? BIANCO : GRIGIO, 1.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
			break;
		}
		if (y >= ly0 && y + ih <= ly1) hit(r, in_game ? SCR_GAME : SCR_SETTINGS, i);
		y += step;
	}
	g_dc->PopAxisAlignedClip();
	if (total > avail)
	{
		// thin scroll bar
		float bh = avail * avail / total, by = ly0 + (avail - bh) * (scroll / (total - avail));
		fill(R(x + w + 6 * S, by, 3 * S, bh), GRIGIO, 0.8f, 1.5f * S);
	}
	// description of the chosen entry and controls
	const SItem &cur = g_items[g_set_sel];
	float dy = H - L.m - 150 * S;
	fill(R(x, dy, w, 1 * S), GRIGIO_SCURO);
	text(T(cur.dit, cur.den), R(x, dy + 14 * S, w, 80 * S), F_SMALL, GRIGIO_CHIARO, 1.f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
	const wchar_t *h = g_pad_input && !in_game ? T(L"L1 R1 Schede   ✕ Scegli   ← → Valore   ○ Chiudi", L"L1 R1 Tabs   ✕ Select   ← → Value   ○ Close")
		: T(L"PagSu PagGiù Schede   Invio Scegli   ← → Valore   Esc Chiudi", L"PgUp PgDn Tabs   Enter Select   ← → Value   Esc Close");
	text(h, R(x, H - L.m - 46 * S, w, 26 * S), F_TINY, GRIGIO);
	(void)t;
}

// in-game menu: game scaled down in the top left (done by the video thread), statistics below, panel on the right
static void draw_game_menu(double t)
{
	Layout L = layout(W, H);
	stroke(R(L.vx - 1, L.vy - 1, L.vw + 2, L.vh + 2), GRIGIO_SCURO, 1.f, 4 * S);
	draw_stat_cards(L, 1.f);
	draw_panel(L, true, t);
}

// settings from the Home screen: same panel, Saturn on the left
static void draw_settings(double t)
{
	Layout L = layout(W, H);
	logo(L.vx + L.vw / 2, L.vy + L.vh / 2, L.vh * 0.8f, 0.9f);
	text(T(L"Impostazioni", L"Settings"), R(L.vx + 40 * S, L.by + 20 * S, L.vw, 60 * S), F_TITLE, BIANCO);
	text(L"SaturnProject " SATURN_VERSION_W, R(L.vx + 44 * S, L.by + 86 * S, L.vw, 30 * S), F_SMALL, GRIGIO);
	draw_panel(L, false, t);
}

// ---------- API ----------
bool ui_init(ID3D11Device *dev)
{
	InitializeCriticalSection(&g_lock);
	g_t0 = now_s();
	D2D1_FACTORY_OPTIONS fo = {};
	if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, (void **)&g_f))) return false;
	IDXGIDevice *dx = nullptr;
	dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dx);
	HRESULT hr = dx ? g_f->CreateDevice(dx, &g_d2dev) : E_FAIL;
	release(dx);
	if (FAILED(hr) || FAILED(g_d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc))) return false;
	g_dc->SetDpi(96, 96);
	g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw);
	g_dc->CreateSolidColorBrush(C_BLACK, &g_br);
	// 100 degree arc on a radius-1 circle (rotated and scaled when drawn)
	if (SUCCEEDED(g_f->CreatePathGeometry(&g_arc)))
	{
		ID2D1GeometrySink *sk = nullptr;
		g_arc->Open(&sk);
		sk->BeginFigure(D2D1::Point2F(1, 0), D2D1_FIGURE_BEGIN_HOLLOW);
		float a = 100.f * 3.14159265f / 180.f;
		sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(cosf(a), sinf(a)), D2D1::SizeF(1, 1), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
		sk->EndFigure(D2D1_FIGURE_END_OPEN);
		sk->Close(); sk->Release();
	}
	// logo from the exe resources (transparent PNG)
	HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
	IWICImagingFactory *wic = nullptr;
	if (rs && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
	{
		void *p = LockResource(LoadResource(nullptr, rs));
		DWORD sz = SizeofResource(nullptr, rs);
		IWICStream *st = nullptr; IWICBitmapDecoder *dec = nullptr; IWICBitmapFrameDecode *fr = nullptr; IWICFormatConverter *cv = nullptr;
		if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE *)p, sz))
			&& SUCCEEDED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))
			&& SUCCEEDED(wic->CreateFormatConverter(&cv))
			&& SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
			g_dc->CreateBitmapFromWicBitmap(cv, nullptr, &g_logo);
		release(cv); release(fr); release(dec); release(st); release(wic);
	}
	return true;
}

void ui_release_targets()
{
	if (g_dc) g_dc->SetTarget(nullptr);
	for (auto &t : g_targets) { release(t.bmp); release(t.tex); }
}

void ui_fini()
{
	ui_release_targets();
	release(g_cache);
	for (auto &f : g_fmt) release(f);
	release(g_logo); release(g_arc); release(g_br); release(g_dw); release(g_dc); release(g_d2dev); release(g_f);
}

static ID2D1Bitmap1 *target_for(ID3D11Texture2D *back)
{
	for (auto &t : g_targets) if (t.tex == back) return t.bmp;
	Target *slot = nullptr;
	for (auto &t : g_targets) if (!t.tex) { slot = &t; break; }
	if (!slot) { ui_release_targets(); slot = &g_targets[0]; }
	IDXGISurface *surf = nullptr;
	back->QueryInterface(__uuidof(IDXGISurface), (void **)&surf);
	if (!surf) return nullptr;
	D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
	ID2D1Bitmap1 *bmp = nullptr;
	g_dc->CreateBitmapFromDxgiSurface(surf, &bp, &bmp);
	surf->Release();
	if (!bmp) return nullptr;
	slot->tex = back; back->AddRef();
	slot->bmp = bmp;
	return bmp;
}

// Over the game the menu and statistics are drawn into a cache image only when needed (input,
// new statistics, at most 4 times per second) and that image is pasted on every frame: redrawing the whole
// panel 120 times per second in 4K slowed presentation down (measured: 85 fps, 1% low 21 with the menu open).
static UINT g_cache_w, g_cache_h;
static double g_cache_t = -10;
static volatile bool g_dirty = true;
static Hit g_cache_hits[64]; static int g_cache_nhits;

static void render_overlay_cache(double t)
{
	if (!g_cache || g_cache_w != (UINT)W || g_cache_h != (UINT)H)
	{
		release(g_cache);
		D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		g_dc->CreateBitmap(D2D1::SizeU((UINT)W, (UINT)H), nullptr, 0, &bp, &g_cache);
		g_cache_w = (UINT)W; g_cache_h = (UINT)H;
		g_dirty = true;
	}
	if (!g_cache || (!g_dirty && t - g_cache_t < 0.25)) return;
	g_dirty = false; g_cache_t = t;
	g_dc->SetTarget(g_cache);
	g_dc->BeginDraw();
	g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
	g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
	g_nhits_draw = 0;
	if (g_menu) draw_game_menu(t);
	else if (g_set.stats) draw_stats();
	memcpy(g_cache_hits, g_hits_draw, sizeof(Hit) * g_nhits_draw); g_cache_nhits = g_nhits_draw;
	g_dc->EndDraw();
	g_dc->SetTarget(nullptr);
}

void ui_draw(ID3D11Texture2D *back, bool video)
{
	if (!g_dc) return;
	ID2D1Bitmap1 *bmp = target_for(back);
	if (!bmp) return;
	D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
	W = (float)d.Width; H = (float)d.Height;
	S = std::min(W / 1920.f, H / 1080.f);
	make_formats();
	double t = now_s();
	bool cached = false;
	EnterCriticalSection(&g_lock);
	if (g_scr == SCR_GAME && (g_menu ? menu_k(t) >= 0.99f : g_set.stats)) { render_overlay_cache(t); cached = g_cache != nullptr; }
	LeaveCriticalSection(&g_lock);
	g_dc->SetTarget(bmp);
	g_dc->BeginDraw();
	g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
	if (!video) g_dc->Clear(C_BLACK);
	EnterCriticalSection(&g_lock);
	g_nhits_draw = 0;
	switch (g_scr)
	{
	case SCR_HOME: draw_home(t); break;
	case SCR_SETTINGS: draw_settings(t); break;
	case SCR_PAIR: draw_pair(t); break;
	case SCR_CONNECT: draw_connect(t); break;
	case SCR_GAME:
		if (cached)
		{
			g_dc->DrawBitmap(g_cache, nullptr, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
			memcpy(g_hits_draw, g_cache_hits, sizeof(Hit) * g_cache_nhits); g_nhits_draw = g_cache_nhits;
		}
		break;
	}
	toast_draw(t);
	memcpy(g_hits, g_hits_draw, sizeof(Hit) * g_nhits_draw); g_nhits = g_nhits_draw;
	LeaveCriticalSection(&g_lock);
	HRESULT hr = g_dc->EndDraw();
	g_dc->SetTarget(nullptr);
	if (hr == D2DERR_RECREATE_TARGET) ui_release_targets();
}

bool ui_overlay_active()
{
	if (g_scr != SCR_GAME) return true;
	return g_menu || g_set.stats || now_s() < g_toast_until;
}

void ui_set_hwnd(HWND h) { g_hwnd = h; }
UiScreen ui_screen() { return g_scr; }
bool ui_menu_open() { return g_menu; }
// with the menu open the pad drives the menu; outside it the pad plays the game
bool ui_captures_pad() { return g_scr != SCR_GAME || g_menu; } // the pad drives the menu while it is open
bool ui_remap_listening() { return g_kchoose && g_tab == T_KEYS && (g_scr == SCR_SETTINGS || (g_scr == SCR_GAME && g_menu)); }
void ui_remap_pressed(int idx)
{
	EnterCriticalSection(&g_lock);
	if (ui_remap_listening() && idx >= 0 && idx < 12 && g_kfocus >= KF_BTN && g_kfocus < KF_RENAME)
	{
		kmap()[g_kfocus - KF_BTN] = idx; g_kchoose = false; changed(); g_dirty = true;
	}
	LeaveCriticalSection(&g_lock);
}

void ui_set_screen(UiScreen s)
{
	EnterCriticalSection(&g_lock);
	g_scr = s; g_menu = false; g_confirm = -1;
	if (s == SCR_HOME) g_home_sel = g_paired ? 0 : H_PAIR;
	if (s == SCR_PAIR) { g_pair_sel = P_CONSOLE; g_pair_msg[0] = 0; start_scan(); }
	LeaveCriticalSection(&g_lock);
}

void ui_set_console(const char *name, const char *host, int state, bool paired)
{
	EnterCriticalSection(&g_lock);
	strncpy(g_cname, name ? name : "", 63); strncpy(g_chost, host ? host : "", 63);
	g_cstate = state; g_paired = paired;
	if (!paired && g_home_sel == H_PLAY) g_home_sel = H_PAIR;
	LeaveCriticalSection(&g_lock);
}

void ui_set_status(const wchar_t *s) { EnterCriticalSection(&g_lock); wcsncpy(g_status, s, 159); LeaveCriticalSection(&g_lock); }
void ui_toast(const wchar_t *s, double sec) { EnterCriticalSection(&g_lock); wcsncpy(g_toast, s, 159); g_toast_until = now_s() + sec; LeaveCriticalSection(&g_lock); }
void ui_error(const wchar_t *s) { EnterCriticalSection(&g_lock); wcsncpy(g_err, s, 199); g_err_until = now_s() + 12; LeaveCriticalSection(&g_lock); }
void ui_set_stats(const UiStats &s) { EnterCriticalSection(&g_lock); g_stats = s; g_have_stats = true; g_dirty = true; LeaveCriticalSection(&g_lock); }

// ---------- commands ----------
static int move_sel(int sel, int dir, int count, bool (*ok)(int))
{
	for (int i = 0; i < count; i++)
	{
		sel = (sel + dir + count) % count;
		if (!ok || ok(sel)) return sel;
	}
	return sel;
}

static bool home_ok(int i) { return i != H_PLAY || g_paired; }
static bool pair_ok(int i) { return pair_visible(i); }

static void open_settings(UiScreen back) { g_settings_back = back; g_scr = SCR_SETTINGS; set_tab(T_PICTURE); }

static void psn_login()
{
	ShellExecuteW(nullptr, L"open", psn_login_url(), nullptr, nullptr, SW_SHOWNORMAL);
	g_psn_wait = true;
	// clear the clipboard so an old address is not read again
	if (OpenClipboard(nullptr)) { EmptyClipboard(); CloseClipboard(); }
	wcscpy(g_pair_msg, T(L"Accedi nel browser, poi copia l'indirizzo della pagina finale.", L"Sign in in the browser, then copy the final page address."));
}

static void do_cmd(UiPad b) // with g_lock held
{
	switch (g_scr)
	{
	case SCR_HOME:
		if (b == UP_UP) g_home_sel = move_sel(g_home_sel, -1, H_COUNT, home_ok);
		else if (b == UP_DOWN) g_home_sel = move_sel(g_home_sel, 1, H_COUNT, home_ok);
		else if (b == UP_OK)
		{
			if (g_home_sel == H_PLAY && g_paired) post(CMD_PLAY);
			else if (g_home_sel == H_SETTINGS) open_settings(SCR_HOME);
			else if (g_home_sel == H_PAIR) { g_scr = SCR_PAIR; g_pair_sel = P_CONSOLE; g_pair_msg[0] = 0; start_scan(); }
			else if (g_home_sel == H_QUIT) post(CMD_QUIT_APP);
		}
		else if (b == UP_BACK) post(CMD_QUIT_APP);
		break;
	case SCR_SETTINGS:
		if (g_tab == T_KEYS && keys_cmd(b)) break;
		if (g_tab == T_KEYS && (b == UP_UP || b == UP_DOWN || b == UP_LEFT || b == UP_RIGHT || b == UP_OK)) break;
		if (b == UP_UP) item_step(-1);
		else if (b == UP_DOWN) item_step(1);
		else if (b == UP_L1) tab_step(-1, false);
		else if (b == UP_R1) tab_step(1, false);
		else if (b == UP_LEFT) item_change(g_set_sel, -1);
		else if (b == UP_RIGHT) item_change(g_set_sel, 1);
		else if (b == UP_OK) item_change(g_set_sel, 0);
		else if (b == UP_BACK || b == UP_MENU) { g_scr = SCR_HOME; g_confirm = -1; }
		break;
	case SCR_PAIR:
		if (b == UP_UP) g_pair_sel = move_sel(g_pair_sel, -1, P_COUNT, pair_ok);
		else if (b == UP_DOWN) g_pair_sel = move_sel(g_pair_sel, 1, P_COUNT, pair_ok);
		else if (g_pair_sel == P_CONSOLE && (b == UP_LEFT || b == UP_RIGHT))
			g_found_sel = (g_found_sel + (b == UP_LEFT ? -1 : 1) + g_nfound + 1) % (g_nfound + 1);
		else if (b == UP_OK)
		{
			if (g_pair_sel == P_CONSOLE) start_scan();
			else if (g_pair_sel == P_PSN && !g_psn_busy) psn_login();
			else if (g_pair_sel == P_GO && !g_pairing)
			{
				g_pairing = true;
				wcscpy(g_pair_msg, T(L"Associazione in corso…", L"Pairing…"));
				CloseHandle(CreateThread(nullptr, 0, pair_thread, nullptr, 0, nullptr));
			}
			else g_pair_sel = move_sel(g_pair_sel, 1, P_COUNT, pair_ok);
		}
		else if (b == UP_BACK) { g_scr = SCR_HOME; g_psn_wait = false; }
		break;
	case SCR_CONNECT:
		if (b == UP_BACK || b == UP_MENU) post(CMD_CANCEL_CONNECT);
		break;
	case SCR_GAME:
		// DualSense Mute (or F1/Esc): ALWAYS opens and closes the menu, even while remapping or typing a name
		if (b == UP_MENU)
		{
			if (g_krename) { g_krename = false; if (!g_set.kp_name[g_set.key_profile][0]) swprintf(g_set.kp_name[g_set.key_profile], 24, L"Profile %d", g_set.key_profile + 1); changed(); }
			g_kchoose = false;
			g_menu = !g_menu;
			if (g_menu) set_tab(T_GAME);
			break;
		}
		if (!g_menu) break;
		if (g_tab == T_KEYS && keys_cmd(b)) break;
		if (g_tab == T_KEYS && (b == UP_UP || b == UP_DOWN || b == UP_LEFT || b == UP_RIGHT || b == UP_OK)) break;
		if (b == UP_MENU || b == UP_BACK) g_menu = false;
		else if (b == UP_UP) item_step(-1);
		else if (b == UP_DOWN) item_step(1);
		else if (b == UP_L1) tab_step(-1, true);
		else if (b == UP_R1) tab_step(1, true);
		else if (b == UP_LEFT) item_change(g_set_sel, -1);
		else if (b == UP_RIGHT) item_change(g_set_sel, 1);
		else if (b == UP_OK) item_change(g_set_sel, 0);
		break;
	}
}

void ui_pad(UiPad b)
{
	EnterCriticalSection(&g_lock);
	g_pad_input = true;
	do_cmd(b);
	g_dirty = true;
	LeaveCriticalSection(&g_lock);
}

static wchar_t *active_field()
{
	if (g_krename && g_tab == T_KEYS) return g_set.kp_name[g_set.key_profile];
	if (g_scr != SCR_PAIR) return nullptr;
	if (g_pair_sel == P_IP) return g_ip;
	if (g_pair_sel == P_ACCOUNT) return g_account;
	if (g_pair_sel == P_PIN) return g_pin;
	return nullptr;
}

static int field_cap(wchar_t *f) { return f == g_ip ? 63 : f == g_account ? 31 : f == g_pin ? 8 : 20; }

static void field_add(wchar_t *f, wchar_t c)
{
	if (f != g_pin && f != g_ip && f != g_account) { size_t n2 = wcslen(f); if ((int)n2 < field_cap(f) && c >= 32) { f[n2] = c; f[n2 + 1] = 0; } return; }
	if (f == g_pin && (c < '0' || c > '9')) return;
	if (f == g_ip && !((c >= '0' && c <= '9') || c == '.')) return;
	if (f == g_account && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) return;
	size_t n = wcslen(f);
	if ((int)n < field_cap(f)) { f[n] = c; f[n + 1] = 0; }
}

void ui_key(WPARAM vk)
{
	EnterCriticalSection(&g_lock);
	g_dirty = true;
	g_pad_input = false;
	bool ctrl = GetKeyState(VK_CONTROL) < 0;
	if (g_krename && (vk == VK_ESCAPE || vk == VK_RETURN)) { do_cmd(UP_OK); LeaveCriticalSection(&g_lock); return; }
	if (g_kchoose && vk == VK_ESCAPE) { g_kchoose = false; LeaveCriticalSection(&g_lock); return; } // cancel waiting for the button
	wchar_t *f = active_field();
	if (f && ctrl && vk == 'V')
	{
		if (OpenClipboard(nullptr))
		{
			HANDLE h = GetClipboardData(CF_UNICODETEXT);
			const wchar_t *s = h ? (const wchar_t *)GlobalLock(h) : nullptr;
			if (s) { for (; *s; s++) field_add(f, *s); GlobalUnlock(h); }
			CloseClipboard();
		}
	}
	else if (f && vk == VK_BACK) { size_t n = wcslen(f); if (n) f[n - 1] = 0; }
	else
	{
		switch (vk)
		{
		case VK_UP: do_cmd(UP_UP); break;
		case VK_DOWN: do_cmd(UP_DOWN); break;
		case VK_LEFT: do_cmd(UP_LEFT); break;
		case VK_RIGHT: do_cmd(UP_RIGHT); break;
		case VK_RETURN: case VK_SPACE: if (!(f && vk == VK_SPACE)) do_cmd(UP_OK); break;
		case VK_PRIOR: do_cmd(UP_L1); break;
		case VK_NEXT: do_cmd(UP_R1); break;
		case VK_ESCAPE: do_cmd(g_scr == SCR_GAME ? UP_MENU : UP_BACK); break;
		case VK_BACK: do_cmd(UP_BACK); break;
		case VK_F1: do_cmd(UP_MENU); break;
		case VK_F2: g_set.stats = !g_set.stats; changed(); break;
		}
	}
	LeaveCriticalSection(&g_lock);
}

void ui_char(wchar_t c)
{
	EnterCriticalSection(&g_lock);
	wchar_t *f = active_field();
	if (f && c >= 32) field_add(f, c);
	LeaveCriticalSection(&g_lock);
}

void ui_mouse(int x, int y, bool click, int wheel)
{
	EnterCriticalSection(&g_lock);
	if (click || wheel) g_dirty = true;
	if (wheel && (g_scr == SCR_SETTINGS || (g_scr == SCR_GAME && g_menu))) { do_cmd(wheel > 0 ? UP_UP : UP_DOWN); }
	for (int i = 0; i < g_nhits; i++)
	{
		const Hit &h = g_hits[i];
		if (x < h.r.left || x > h.r.right || y < h.r.top || y > h.r.bottom || h.screen != g_scr) continue;
		if (g_scr == SCR_GAME && !g_menu) break;
		g_pad_input = false;
		switch (g_scr)
		{
		case SCR_HOME: if (home_ok(h.idx)) g_home_sel = h.idx; break;
		case SCR_SETTINGS: case SCR_GAME:
			if (h.idx >= 2000) { g_kfocus = h.idx - 2000; g_kchoose = false; if (click) keys_activate(true); click = false; }
			else if (h.idx >= 1000) { if (click) set_tab(h.idx - 1000); click = false; }
			else if (g_set_sel != h.idx) { g_confirm = -1; g_set_sel = h.idx; }
			break;
		case SCR_PAIR: g_pair_sel = h.idx; break;
		default: break;
		}
		if (click) do_cmd(UP_OK);
		break;
	}
	LeaveCriticalSection(&g_lock);
}

// every ~100 ms from the main thread: during the PSN sign-in watch the clipboard
void ui_tick()
{
	EnterCriticalSection(&g_lock);
	bool wait = g_psn_wait && !g_psn_busy && g_scr == SCR_PAIR;
	LeaveCriticalSection(&g_lock);
	if (!wait) return;
	char url[2048] = {};
	if (OpenClipboard(nullptr))
	{
		HANDLE h = GetClipboardData(CF_UNICODETEXT);
		const wchar_t *s = h ? (const wchar_t *)GlobalLock(h) : nullptr;
		if (s) { snprintf(url, sizeof(url), "%ls", s); GlobalUnlock(h); }
		CloseClipboard();
	}
	if (!strstr(url, "remoteplay/redirect") || !strstr(url, "code=")) return;
	EnterCriticalSection(&g_lock);
	g_psn_wait = false; g_psn_busy = true;
	strcpy(g_redirect, url);
	wcscpy(g_pair_msg, T(L"Indirizzo ricevuto, leggo l'account…", L"Address received, reading the account…"));
	LeaveCriticalSection(&g_lock);
	CloseHandle(CreateThread(nullptr, 0, psn_thread, nullptr, 0, nullptr));
}

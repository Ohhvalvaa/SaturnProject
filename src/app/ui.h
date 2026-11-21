// User interface: Direct2D/DirectWrite drawn straight into the swapchain image (over the game or on black).
// Model + drawing live here; the real actions (play, quit, pair) are done by main.cpp, which receives
// WM_APP_UI messages on the window.
#pragma once
#include <windows.h>
#include <d3d11.h>

enum UiScreen { SCR_HOME, SCR_SETTINGS, SCR_PAIR, SCR_CONNECT, SCR_GAME };
enum UiPad { UP_UP, UP_DOWN, UP_LEFT, UP_RIGHT, UP_OK, UP_BACK, UP_MENU, UP_L1, UP_R1 };

// messages to the main window: WM_APP_UI with wParam = UiCmd
#define WM_APP_UI (WM_APP + 20)
enum UiCmd {
	CMD_PLAY,            // start playing
	CMD_CANCEL_CONNECT,  // cancel wake-up/connection
	CMD_LEAVE_REST,      // leave the game, PS5 to rest mode
	CMD_LEAVE_ON,        // leave the game, PS5 stays on
	CMD_QUIT_APP,        // close the app
	CMD_SETTINGS_CHANGED,// settings changed (lParam = which group: 0 all)
	CMD_PAIRED,          // pairing succeeded: reload the console
	CMD_FORGET,          // forget the PS5
};

bool ui_init(ID3D11Device *dev);
void ui_fini();
void ui_release_targets(); // before ResizeBuffers
// draws over 'back' (swapchain image). video = the game is underneath. Called from the video thread.
void ui_draw(ID3D11Texture2D *back, bool video);
bool ui_overlay_active();   // something to draw over the game (menu, statistics, notice)
// rectangle (output pixels) for the game while the menu is open: false = full screen. From the video thread.
bool ui_video_rect(int out_w, int out_h, RECT *r);

void ui_set_hwnd(HWND h);
void ui_set_screen(UiScreen s);
UiScreen ui_screen();
void ui_set_console(const char *name, const char *host, int state, bool paired); // state: ConsoleState
void ui_set_status(const wchar_t *s);   // connection screen text
void ui_toast(const wchar_t *s, double seconds);
void ui_error(const wchar_t *s);        // message on the Home screen (e.g. "The PS5 does not answer")

// statistics (once per second from the main thread)
struct UiStats
{
	double fps_shown, fps_source, fps_dups, fps_generated, fg_ms, decode_ms;
	double rtt_ms, mbit, loss;
	int src_w, src_h, out_w, out_h, refresh;
	bool vsr, fg, h265;
	long dropped, errors;
	int av_auto;              // automatic audio delay (ms) for frame generation
	double repeats;           // display refreshes without a new image, per second
	const char *fg_engine;
};
void ui_set_stats(const UiStats &s);

// input
void ui_key(WPARAM vk);     // WM_KEYDOWN
void ui_char(wchar_t c);    // WM_CHAR
void ui_pad(UiPad b);       // from the pad (SDL thread)
void ui_mouse(int x, int y, bool click, int wheel);
bool ui_captures_pad();     // true = the pad drives the UI (no input to the PS5)
bool ui_menu_open();
// remapping: the app waits for the button to send to be pressed on the pad (only then the pad belongs to the UI)
bool ui_remap_listening();
void ui_remap_pressed(int idx); // 0-11: Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Create
void ui_tick();             // periodic checks (clipboard for the PSN sign-in), from the main thread

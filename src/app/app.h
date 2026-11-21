// SaturnProject: application modules. Each module is independent and talks to the chiaki session
// only through these functions.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <chiaki/session.h>

#define SATURN_VERSION_W L"1.0.0"

// ---------- video (video.cpp): D3D11VA + frame generation + flip swapchain ----------
bool video_init(HWND hwnd);           // device, swapchain, UI and presentation thread
void video_fini();
void video_set_refresh(double hz);
bool video_begin(bool h265);          // new session: decoder ready
void video_end();                     // session ended
// chiaki session callback: one compressed frame
bool video_sample(uint8_t *buf, size_t size, int32_t frames_lost, bool recovered, void *user);
void video_resize(int w, int h);
struct VideoStats
{
	long decoded, presented, dropped, errors, dups, generated, real_shown;
	double decode_ms, fruc_ms, avg_packet; long vsyncs;
	long long bytes;
	int src_w, src_h, out_w, out_h;
	const char *engine;
	double refresh;
};
VideoStats video_stats();
int video_frame_times(float *out_ms, int max, double window_s);
void video_screenshot();             // screenshot (PNG in Pictures\SaturnProject) shortly
int video_av_delay_ms();              // picture delay caused by frame generation: audio adds the same

// ---------- audio (audio.cpp): Opus -> WASAPI ----------
void audio_attach(ChiakiSession *session, ChiakiLog *log);
void audio_fini();

// ---------- pad (pad.cpp): SDL3 -> chiaki controller state ----------
bool pad_init();                          // SDL thread (also on the Home screen: the pad drives the UI)
void pad_fini();
void pad_attach(ChiakiSession *session);  // null = no session
void pad_event(ChiakiEvent *ev);          // rumble, adaptive triggers, light bar, motion reset
void pad_haptics(uint8_t *buf, size_t size); // PS5 haptics -> rumble
const char *pad_name();                   // connected pad name ("" if none)
int pad_battery(bool *charging);          // pad battery in % (-1 = unknown/wired)

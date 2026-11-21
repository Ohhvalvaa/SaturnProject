// Audio: Opus from the PS5 (decoded by libchiaki) -> shared WASAPI on the default device.
// Short queue (target ~30 ms): if delay builds up a packet is dropped, so audio stays
// glued to the picture instead of drifting behind.
#include "app.h"
#include "settings.h"
#include <chiaki/opusdecoder.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

static ChiakiOpusDecoder g_opus;
static IAudioClient *g_client;
static IAudioRenderClient *g_render;
static UINT32 g_buffer_frames, g_rate, g_channels;
static bool g_started;
static const UINT32 TARGET_MS = 30, MAX_MS = 60;

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static void com_thread()
{
	static thread_local bool done;
	if (!done) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); done = true; }
}

static void settings_cb(uint32_t channels, uint32_t rate, void *)
{
	com_thread();
	release(g_render);
	if (g_client) { g_client->Stop(); release(g_client); }
	g_started = false;
	IMMDeviceEnumerator *en = nullptr; IMMDevice *dev = nullptr;
	CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en);
	if (en) en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
	if (dev) dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&g_client);
	release(dev); release(en);
	if (!g_client) { fprintf(stderr, "Audio: no device\n"); return; }
	WAVEFORMATEX wf = {};
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = (WORD)channels;
	wf.nSamplesPerSec = rate;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = (WORD)(channels * 2);
	wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
	HRESULT hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
		3000000 /* 300 ms buffer: 30 + chosen delay (max 200) */, 0, &wf, nullptr);
	if (FAILED(hr)) { fprintf(stderr, "Audio: Initialize 0x%08lx\n", hr); release(g_client); return; }
	g_client->GetBufferSize(&g_buffer_frames);
	g_client->GetService(__uuidof(IAudioRenderClient), (void **)&g_render);
	g_rate = rate; g_channels = channels;
}

static void frame_cb(int16_t *buf, size_t samples, void *)
{
	if (!g_render) return;
	com_thread();
	// volume from the settings (100 = unchanged)
	int vol = g_set.volume;
	if (vol < 100) for (size_t i = 0; i < samples * g_channels; i++) buf[i] = (int16_t)(buf[i] * vol / 100);
	// wanted delay = the one chosen in the settings + the picture delay caused by frame generation
	UINT32 extra = (UINT32)g_set.audio_delay + (UINT32)video_av_delay_ms();
	UINT32 target = TARGET_MS + extra, maxms = MAX_MS + extra;
	UINT32 padding = 0;
	if (FAILED(g_client->GetCurrentPadding(&padding))) return;
	if (g_started && padding > g_rate * maxms / 1000) return; // too much delay: drop this packet
	// chosen delay (> 0): if the queue is more than 10 ms below the target, silence is added up to the target.
	// This way the delay can be adjusted while playing (and a network gap does not shorten the wanted delay).
	// With 0 (default) the queue stays minimal.
	if (g_started && extra > 0)
	{
		UINT32 want = g_rate * target / 1000;
		if (padding + g_rate / 100 < want && want <= g_buffer_frames)
		{
			UINT32 add = want - padding;
			BYTE *sil = nullptr;
			if (SUCCEEDED(g_render->GetBuffer(add, &sil))) { g_render->ReleaseBuffer(add, AUDCLNT_BUFFERFLAGS_SILENT); padding += add; }
		}
	}
	if (padding + samples > g_buffer_frames) return;
	BYTE *dst = nullptr;
	if (FAILED(g_render->GetBuffer((UINT32)samples, &dst))) return;
	memcpy(dst, buf, samples * g_channels * sizeof(int16_t));
	g_render->ReleaseBuffer((UINT32)samples, 0);
	if (!g_started && padding + samples >= g_rate * target / 1000) { g_client->Start(); g_started = true; }
}

void audio_attach(ChiakiSession *session, ChiakiLog *log)
{
	chiaki_opus_decoder_init(&g_opus, log);
	chiaki_opus_decoder_set_cb(&g_opus, settings_cb, frame_cb, nullptr);
	ChiakiAudioSink sink;
	chiaki_opus_decoder_get_sink(&g_opus, &sink);
	chiaki_session_set_audio_sink(session, &sink);
}

void audio_fini()
{
	if (g_client) g_client->Stop();
	release(g_render); release(g_client);
	chiaki_opus_decoder_fini(&g_opus);
}

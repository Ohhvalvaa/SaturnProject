// Video: H.265/H.264 from the PS5 -> GPU decode (FFmpeg D3D11VA) -> duplicate removal -> in-between frames
// (DLSS / FSR 3 frame generation, or NVOF optical flow + our shader) -> frame pacer -> D3D11 Video Processor (color +
// scaling, RTX Video Super Resolution) -> Direct2D UI on top -> flip swapchain.
// No copies to system memory: frames always stay on the GPU.
// The presentation thread lives as long as the app: without a game it only draws the UI (Home, settings).
#include "app.h"
#include "framegen.h"
#include "ofi.h"
#include "dlssg.h"
#include "settings.h"
#include "ui.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <shlobj.h>
#include <time.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

static HWND g_hwnd;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11DeviceContext4 *g_ctx4;
static ID3D11VideoDevice *g_vdev;
static ID3D11VideoContext1 *g_vctx;
static IDXGISwapChain2 *g_swap;
static HANDLE g_waitable;
static bool g_tearing;
static AVBufferRef *g_hwdev;
static AVCodecContext *g_codec;
static AVPacket *g_pkt;
static AVFrame *g_recv;
static CRITICAL_SECTION g_codec_lock;

// Surfaces just shown are held back before returning to FFmpeg: the surface pool hands out first the last one
// released, which the GPU is still reading (presentation queue), and the D3D11VA decoder then waits 2 ms at a time
// (E_PENDING). Measured: 6 ms decode with a 60 fps source, broken pacing. Holding 3 back it gets free surfaces.
static const int HOLD = 3;

// queue of decoded frames
struct Entry { AVFrame *f; double ts; bool copy; float ch; }; // ch: pixels changed against the previous frame (-1 = not measured)
static float small_changed(ID3D11Texture2D *tex, UINT slice, int w, int h); // copy: the packet looked like a duplicate of the previous one
static const int QMAX = 4;
static Entry g_q[QMAX];
static int g_qn;
static CRITICAL_SECTION g_lock;
static HANDLE g_ready_event, g_presenter, g_stop_event;
static volatile LONG g_decoded, g_presented, g_dropped, g_errors, g_dups, g_generated, g_real_shown;
static volatile LONG g_session_gen;
static double g_decode_ms, g_fruc_ms;
static int g_out_w, g_out_h, g_src_w, g_src_h;
static volatile LONG g_resize_pending;
static double g_refresh = 60.0;
static bool g_fg_available;   // suitable device (ID3D11DeviceContext4)
static bool g_fruc_dead;      // FRUC failed: do not retry
static bool g_nvof_dead;      // NVOF not available
static bool g_dlss_dead;      // DLSS Frame Generation not available: fall back to the next engine
static bool g_fsr_dead;       // AMD FSR 3 not available
static UINT g_latency_set;
static Fruc g_fa;
static int g_fw, g_fh;
static const char *g_engine_name = "";
static volatile double g_last_video_present;
static volatile bool g_streaming;
static volatile LONG g_av_delay_ms; // extra picture delay caused by frame generation (audio follows it)

static double g_ema_big = 0;
static volatile LONG64 g_bytes_total;

// Video Processor (recreated if the input or output size changes)
static ID3D11VideoProcessorEnumerator *g_vpe;
static ID3D11VideoProcessor *g_vp;
static int g_vp_in_w, g_vp_in_h, g_vp_out_w, g_vp_out_h;
static bool g_vp_rgb, g_vp_vsr;
static RECT g_vp_dst; // current game rectangle in the output (changes while the menu is open)

// Present times (for the panel: fps, 1% low, pacing graph)
static const int PT_N = 1024;
static double g_pt[PT_N];
static volatile LONG g_pti;

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static double now_s()
{
	static LARGE_INTEGER fq; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
	LARGE_INTEGER t; QueryPerformanceCounter(&t);
	return (double)t.QuadPart / fq.QuadPart;
}

static enum AVPixelFormat get_format(AVCodecContext *, const enum AVPixelFormat *fmts)
{
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == AV_PIX_FMT_D3D11) return *p;
	fprintf(stderr, "D3D11VA not available for this stream\n");
	return AV_PIX_FMT_NONE;
}

static void hw_lock() { auto *d = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)g_hwdev->data)->hwctx; d->lock(d->lock_ctx); }
static void hw_unlock() { auto *d = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)g_hwdev->data)->hwctx; d->unlock(d->lock_ctx); }

static bool create_swapchain()
{
	IDXGIDevice *dxgidev = nullptr; IDXGIAdapter *ad = nullptr; IDXGIFactory5 *f = nullptr;
	g_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgidev);
	dxgidev->GetAdapter(&ad);
	ad->GetParent(__uuidof(IDXGIFactory5), (void **)&f);
	BOOL tearing = FALSE;
	f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
	g_tearing = tearing;
	RECT rc; GetClientRect(g_hwnd, &rc);
	g_out_w = rc.right > 0 ? rc.right : 1; g_out_h = rc.bottom > 0 ? rc.bottom : 1;
	DXGI_SWAP_CHAIN_DESC1 d = {};
	d.Width = g_out_w; d.Height = g_out_h;
	d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	d.BufferCount = 3;
	d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	IDXGISwapChain1 *sc1 = nullptr;
	HRESULT hr = f->CreateSwapChainForHwnd(g_dev, g_hwnd, &d, nullptr, nullptr, &sc1);
	f->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
	release(f); release(ad); release(dxgidev);
	if (FAILED(hr)) return false;
	sc1->QueryInterface(__uuidof(IDXGISwapChain2), (void **)&g_swap);
	release(sc1);
	g_swap->SetMaximumFrameLatency(1);
	g_latency_set = 1;
	g_waitable = g_swap->GetFrameLatencyWaitableObject();
	return true;
}

static DWORD WINAPI presenter(void *);

bool video_init(HWND hwnd)
{
	g_hwnd = hwnd;
	InitializeCriticalSection(&g_lock);
	InitializeCriticalSection(&g_codec_lock);
	g_ready_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	g_stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);

	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
		return false;
	{
		// optional: GPU priority for our work (decode, generation, presentation)
		IDXGIDevice *xd = nullptr;
		if (getenv("SATURN_GPU_PRIO") && SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd))) { xd->SetGPUThreadPriority(7); xd->Release(); }
	}
	ID3D10Multithread *mt = nullptr;
	if (SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
	g_dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&g_vdev);
	g_ctx->QueryInterface(__uuidof(ID3D11VideoContext1), (void **)&g_vctx);
	g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&g_ctx4);
	if (!g_vdev || !g_vctx || !create_swapchain()) return false;

	// FFmpeg uses the same D3D11 device: decoded surfaces go straight to the Video Processor
	g_hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	auto *hw = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)g_hwdev->data)->hwctx;
	hw->device = g_dev; g_dev->AddRef();
	hw->device_context = g_ctx; g_ctx->AddRef();
	if (av_hwdevice_ctx_init(g_hwdev) < 0) return false;
	g_pkt = av_packet_alloc();
	g_recv = av_frame_alloc();
	g_fg_available = g_ctx4 != nullptr;
	if (!ui_init(g_dev)) fprintf(stderr, "Direct2D UI not available\n");
	g_presenter = CreateThread(nullptr, 0, presenter, nullptr, 0, nullptr);
	return true;
}

void video_set_refresh(double hz) { g_refresh = hz > 0 ? hz : 60.0; }

bool video_begin(bool h265)
{
	EnterCriticalSection(&g_codec_lock);
	if (g_codec) avcodec_free_context(&g_codec);
	const AVCodec *codec = avcodec_find_decoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	g_codec = avcodec_alloc_context3(codec);
	g_codec->hw_device_ctx = av_buffer_ref(g_hwdev);
	g_codec->get_format = get_format;
	g_codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	g_codec->flags2 |= AV_CODEC_FLAG2_FAST;
	g_codec->extra_hw_frames = QMAX + 2 + HOLD;
	bool ok = avcodec_open2(g_codec, codec, nullptr) >= 0;
	if (!ok) avcodec_free_context(&g_codec);
	LeaveCriticalSection(&g_codec_lock);
	InterlockedIncrement(&g_session_gen);
	g_ema_big = 0;
	g_src_w = g_src_h = 0;
	g_streaming = ok;
	fprintf(stderr, "Video: %s, display %.0f Hz\n", h265 ? "H.265" : "H.264", g_refresh);
	return ok;
}

void video_end()
{
	g_streaming = false;
	EnterCriticalSection(&g_codec_lock);
	if (g_codec) avcodec_free_context(&g_codec);
	LeaveCriticalSection(&g_codec_lock);
	EnterCriticalSection(&g_lock);
	for (int i = 0; i < g_qn; i++) av_frame_free(&g_q[i].f);
	g_qn = 0;
	LeaveCriticalSection(&g_lock);
	InterlockedIncrement(&g_session_gen);
}

bool video_sample(uint8_t *buf, size_t size, int32_t frames_lost, bool recovered, void *)
{
	(void)frames_lost; (void)recovered;
	double ts = now_s();
	LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
	InterlockedAdd64(&g_bytes_total, (LONG64)size);

	{
		// diagnostics: SATURN_DUMP_SIZES=<skip N frames> logs the size of the next 600
		static bool init; static long n, skip = -1;
		if (!init) { init = true; char v[16] = { 0 }; if (GetEnvironmentVariableA("SATURN_DUMP_SIZES", v, 16) > 0) skip = atol(v); }
		if (skip >= 0 && n >= skip && n < skip + 600) fprintf(stderr, "pkt %ld %zu\n", n, size);
		n++;
	}
	// Duplicates: a 30 fps game arrives at 60 repeating every image; the copy, encoded as "no
	// change", is a tiny packet. Measured on a 30 fps game (H.265 50 Mbit/s): 15 KB, 1.5-5 KB, 15 KB...
	// The copy weighs < 45% of the previous packet. This only gates the GPU comparison (score),
	// so a 60 fps game with an isolated light frame is never compared nor dropped.
	static size_t last_size;
	static bool last_cand;
	static int score;
	static LONG gen = -1;
	if (gen != g_session_gen) { gen = g_session_gen; last_size = 0; last_cand = false; score = 0; }
	bool cand = last_size > 0 && size < last_size * 0.45 && !last_cand; bool maybe_copy = cand && score >= 1; // only light packets inside the light/heavy pattern of 30 fps games are compared on the GPU (never in 60 fps games)
	if (cand) score = score < 10 ? score + 1 : 10;
	else if (!last_cand) score = score > 2 ? score - 2 : 0; // two full frames in a row: no 30 fps pattern
	// packet size alone is never used to drop frames (it is wrong when the game moves a lot): the real decision is
	// taken on the GPU by comparing the images (present_with_fg)
	bool dup = false;
	last_cand = cand;
	last_size = size;
	if (!cand) g_ema_big = g_ema_big > 0 ? g_ema_big * 0.9 + 0.1 * size : (double)size;

	EnterCriticalSection(&g_codec_lock);
	if (!g_codec) { LeaveCriticalSection(&g_codec_lock); return false; }
	g_pkt->data = buf;
	g_pkt->size = (int)size;
	int r = avcodec_send_packet(g_codec, g_pkt);
	if (r < 0 && r != AVERROR(EAGAIN)) { LeaveCriticalSection(&g_codec_lock); InterlockedIncrement(&g_errors); return false; }
	while ((r = avcodec_receive_frame(g_codec, g_recv)) == 0)
	{
		InterlockedIncrement(&g_decoded);
		g_src_w = g_recv->width; g_src_h = g_recv->height;
		if (dup) { InterlockedIncrement(&g_dups); av_frame_unref(g_recv); continue; }
		// duplicates in 30 fps games: comparison with the previous image here, in the decode thread (on a 480x272
		// copy), so the presentation thread never waits for the GPU
		static int nodedup = -1; if (nodedup < 0) nodedup = getenv("SATURN_NO_DEDUP") != nullptr;
		float ch = !nodedup && maybe_copy && g_set.fg != FG_OFF && !g_set.competitive ? small_changed((ID3D11Texture2D *)g_recv->data[0], (UINT)(intptr_t)g_recv->data[1], g_recv->width, g_recv->height) : -1;
		EnterCriticalSection(&g_lock);
		if (g_qn == QMAX) { av_frame_free(&g_q[0].f); memmove(g_q, g_q + 1, sizeof(Entry) * (QMAX - 1)); g_qn--; InterlockedIncrement(&g_dropped); }
		AVFrame *f = av_frame_alloc();
		av_frame_move_ref(f, g_recv);
		g_q[g_qn++] = { f, ts, cand, ch };
		LeaveCriticalSection(&g_lock);
		SetEvent(g_ready_event);
	}
	LeaveCriticalSection(&g_codec_lock);
	QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
	g_decode_ms = g_decode_ms * 0.95 + 0.05 * (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
	return r == AVERROR(EAGAIN) || r == 0;
}

// RTX Video Super Resolution: NVIDIA Video Processor extension (like mpv and VLC)
static void set_rtx_vsr(bool on)
{
	static const GUID nv = { 0xd43ce1b3, 0x1f4b, 0x48ac, { 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 } };
	struct { UINT version, method, enable; } s = { 0x1, 0x2, on ? 0x1u : 0x0u };
	HRESULT hr = g_vctx->VideoProcessorSetStreamExtension(g_vp, 0, &nv, sizeof(s), &s);
	static int told = -1;
	if (told != (int)on) { told = on; fprintf(stderr, "RTX Video Super Resolution: %s\n", !on ? "off" : SUCCEEDED(hr) ? "requested" : "not available"); }
}

static bool ensure_vp(int in_w, int in_h, bool rgb)
{
	bool vsr = g_set.vsr;
	if (g_vp && in_w == g_vp_in_w && in_h == g_vp_in_h && g_out_w == g_vp_out_w && g_out_h == g_vp_out_h && rgb == g_vp_rgb && vsr == g_vp_vsr) return true;
	g_vp_rgb = rgb; g_vp_vsr = vsr;
	release(g_vp); release(g_vpe);
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
	cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	cd.InputWidth = in_w; cd.InputHeight = in_h;
	cd.OutputWidth = g_out_w; cd.OutputHeight = g_out_h;
	cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
	if (FAILED(g_vdev->CreateVideoProcessorEnumerator(&cd, &g_vpe))) return false;
	if (FAILED(g_vdev->CreateVideoProcessor(g_vpe, 0, &g_vp))) return false;
	g_vp_in_w = in_w; g_vp_in_h = in_h; g_vp_out_w = g_out_w; g_vp_out_h = g_out_h;

	RECT src = { 0, 0, in_w, in_h };
	RECT full = { 0, 0, g_out_w, g_out_h };
	g_vp_dst = {}; // the game rectangle is set on every frame (place_video)
	g_vctx->VideoProcessorSetStreamFrameFormat(g_vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	g_vctx->VideoProcessorSetStreamSourceRect(g_vp, 0, TRUE, &src);
	g_vctx->VideoProcessorSetOutputTargetRect(g_vp, TRUE, &full);
	D3D11_VIDEO_COLOR black = {}; black.RGBA.A = 1.0f;
	g_vctx->VideoProcessorSetOutputBackgroundColor(g_vp, FALSE, &black);
	g_vctx->VideoProcessorSetStreamAutoProcessingMode(g_vp, 0, FALSE);
	g_vctx->VideoProcessorSetStreamColorSpace1(g_vp, 0, rgb ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
	g_vctx->VideoProcessorSetOutputColorSpace1(g_vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	set_rtx_vsr(vsr);
	return true;
}

// 16:9 image centered in the rectangle: the whole screen, or the small one with the menu open (black bars around)
static void place_video(int in_w, int in_h)
{
	RECT box = { 0, 0, g_out_w, g_out_h };
	ui_video_rect(g_out_w, g_out_h, &box);
	int bw = box.right - box.left, bh = box.bottom - box.top;
	int dw = bw, dh = bw * in_h / in_w;
	if (dh > bh) { dh = bh; dw = bh * in_w / in_h; }
	RECT dst = { box.left + (bw - dw) / 2, box.top + (bh - dh) / 2, box.left + (bw - dw) / 2 + dw, box.top + (bh - dh) / 2 + dh };
	if (memcmp(&dst, &g_vp_dst, sizeof(RECT)) == 0) return;
	g_vp_dst = dst;
	g_vctx->VideoProcessorSetStreamDestRect(g_vp, 0, TRUE, &dst);
}

// NV12 -> RGBA/BGRA conversion at the stream resolution (for generation): second Video Processor, no scaling.
static ID3D11VideoProcessorEnumerator *g_cvpe;
static ID3D11VideoProcessor *g_cvp;
static ID3D11Texture2D *g_conv;
static int g_cw, g_ch;
static DXGI_FORMAT g_conv_fmt = DXGI_FORMAT_R8G8B8A8_UNORM, g_conv_made; // RGBA for FRUC, BGRA for NVOF/DLSS/FSR

static ID3D11Texture2D *convert_rgba(ID3D11Texture2D *tex, UINT slice, int w, int h)
{
	hw_lock();
	if (!g_cvp || w != g_cw || h != g_ch || g_conv_fmt != g_conv_made)
	{
		release(g_cvp); release(g_cvpe); release(g_conv);
		g_cw = w; g_ch = h; g_conv_made = g_conv_fmt;
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
		cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		cd.InputWidth = w; cd.InputHeight = h; cd.OutputWidth = w; cd.OutputHeight = h;
		cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
		d.Format = g_conv_fmt; d.SampleDesc.Count = 1;
		d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(g_vdev->CreateVideoProcessorEnumerator(&cd, &g_cvpe)) || FAILED(g_vdev->CreateVideoProcessor(g_cvpe, 0, &g_cvp))
			|| FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_conv)))
		{
			hw_unlock();
			return nullptr;
		}
		g_vctx->VideoProcessorSetStreamFrameFormat(g_cvp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
		g_vctx->VideoProcessorSetStreamAutoProcessingMode(g_cvp, 0, FALSE);
		g_vctx->VideoProcessorSetStreamColorSpace1(g_cvp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
		g_vctx->VideoProcessorSetOutputColorSpace1(g_cvp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	}
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od = {}; od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	ID3D11VideoProcessorOutputView *ov = nullptr;
	g_vdev->CreateVideoProcessorOutputView(g_conv, g_cvpe, &od, &ov);
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id = {}; id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; id.Texture2D.ArraySlice = slice;
	ID3D11VideoProcessorInputView *iv = nullptr;
	g_vdev->CreateVideoProcessorInputView(tex, g_cvpe, &id, &iv);
	bool ok = ov && iv;
	if (ok)
	{
		D3D11_VIDEO_PROCESSOR_STREAM st = {}; st.Enable = TRUE; st.pInputSurface = iv;
		ok = SUCCEEDED(g_vctx->VideoProcessorBlt(g_cvp, ov, 0, 1, &st));
	}
	release(iv); release(ov);
	hw_unlock();
	return ok ? g_conv : nullptr;
}

// ---------- fast comparison with the previous image (decode thread) ----------
static ID3D11VideoProcessorEnumerator *g_svpe;
static ID3D11VideoProcessor *g_svp;
static ID3D11Texture2D *g_small, *g_small_st;
static int g_sin_w, g_sin_h;
static const int SMW = 480, SMH = 272;
static uint8_t g_sprev[SMW * SMH];
static bool g_shave;

static float small_changed(ID3D11Texture2D *tex, UINT slice, int w, int h)
{
	hw_lock();
	if (!g_svp || w != g_sin_w || h != g_sin_h)
	{
		release(g_svp); release(g_svpe); release(g_small); release(g_small_st);
		g_sin_w = w; g_sin_h = h; g_shave = false;
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
		cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		cd.InputWidth = w; cd.InputHeight = h; cd.OutputWidth = SMW; cd.OutputHeight = SMH;
		cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
		D3D11_TEXTURE2D_DESC d = {}; d.Width = SMW; d.Height = SMH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
		d.BindFlags = D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC sd = d; sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(g_vdev->CreateVideoProcessorEnumerator(&cd, &g_svpe)) || FAILED(g_vdev->CreateVideoProcessor(g_svpe, 0, &g_svp))
			|| FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_small)) || FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_small_st)))
		{ release(g_svp); hw_unlock(); return -1; }
		g_vctx->VideoProcessorSetStreamFrameFormat(g_svp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
		g_vctx->VideoProcessorSetStreamAutoProcessingMode(g_svp, 0, FALSE);
		g_vctx->VideoProcessorSetStreamColorSpace1(g_svp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
		g_vctx->VideoProcessorSetOutputColorSpace1(g_svp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	}
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od = {}; od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	ID3D11VideoProcessorOutputView *ov = nullptr;
	g_vdev->CreateVideoProcessorOutputView(g_small, g_svpe, &od, &ov);
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id = {}; id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; id.Texture2D.ArraySlice = slice;
	ID3D11VideoProcessorInputView *iv = nullptr;
	g_vdev->CreateVideoProcessorInputView(tex, g_svpe, &id, &iv);
	bool ok = ov && iv;
	if (ok)
	{
		D3D11_VIDEO_PROCESSOR_STREAM st = {}; st.Enable = TRUE; st.pInputSurface = iv;
		ok = SUCCEEDED(g_vctx->VideoProcessorBlt(g_svp, ov, 0, 1, &st));
		if (ok) { g_ctx->CopyResource(g_small_st, g_small); g_ctx->Flush(); }
	}
	release(iv); release(ov);
	hw_unlock();
	if (!ok) return -1;
	// the result is awaited here, releasing the context between attempts (the presentation thread keeps going)
	float res = -1;
	for (int i = 0; i < 300; i++)
	{
		D3D11_MAPPED_SUBRESOURCE m;
		hw_lock();
		HRESULT hr = g_ctx->Map(g_small_st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (SUCCEEDED(hr))
		{
			int changed = 0;
			for (int y = 0; y < SMH; y++)
			{
				const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
				uint8_t *pv = g_sprev + y * SMW;
				for (int x = 0; x < SMW; x++)
				{
					int l = (row[x * 4] * 29 + row[x * 4 + 1] * 150 + row[x * 4 + 2] * 77) >> 8;
					if (abs(l - pv[x]) > 6) changed++;
					pv[x] = (uint8_t)l;
				}
			}
			g_ctx->Unmap(g_small_st, 0);
			hw_unlock();
			res = g_shave ? (float)changed / (SMW * SMH) : -1;
			g_shave = true;
			break;
		}
		hw_unlock();
		if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
		Sleep(i < 20 ? 0 : 1);
	}
	return res;
}

// ---------- test recording: SATURN_REC=<seconds> records the next 240 shown images ----------
// Each image (RGBA/BGRA, stream resolution) goes to RAM with its Present time and type (real/generated),
// then to disk as rec_NNN.raw + rec.txt. GPU copies to a staging ring read 2 frames later: no stalls.
static const int REC_N = 240, REC_RING = 4;
static ID3D11Texture2D *g_rec_st[REC_RING];
static uint8_t *g_rec_buf[REC_N];
static double g_rec_t[REC_N];
static int g_rec_kind[REC_N];
static int g_rec_i = -1, g_rec_w, g_rec_h;
static double g_rec_at = -1;
static int g_rec_kind_next;

static void rec_flush_one(int i)
{
	ID3D11Texture2D *st = g_rec_st[i % REC_RING];
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) return;
	g_rec_buf[i] = (uint8_t *)malloc((size_t)g_rec_w * g_rec_h * 4);
	for (int y = 0; y < g_rec_h; y++) memcpy(g_rec_buf[i] + (size_t)y * g_rec_w * 4, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)g_rec_w * 4);
	g_ctx->Unmap(st, 0);
}

static void rec_write_all()
{
	char dir[MAX_PATH]; snprintf(dir, sizeof(dir), "%s\\rec", settings_dir());
	CreateDirectoryA(dir, nullptr);
	char p[MAX_PATH]; snprintf(p, sizeof(p), "%s\\rec.txt", dir);
	FILE *t = fopen(p, "w");
	for (int i = 0; i < REC_N; i++)
	{
		if (t) fprintf(t, "%d %.6f %d\n", i, g_rec_t[i], g_rec_kind[i]);
		if (!g_rec_buf[i]) continue;
		snprintf(p, sizeof(p), "%s\\rec_%03d.raw", dir, i);
		FILE *f = fopen(p, "wb");
		if (f) { fwrite(g_rec_buf[i], 1, (size_t)g_rec_w * g_rec_h * 4, f); fclose(f); }
		free(g_rec_buf[i]); g_rec_buf[i] = nullptr;
	}
	if (t) { fprintf(t, "size %d %d\n", g_rec_w, g_rec_h); fclose(t); }
	fprintf(stderr, "Recording saved in %s\n", dir);
}

// called with hw_lock held
static void rec_frame(ID3D11Texture2D *tex, int w, int h, bool rgb)
{
	if (g_rec_at < 0) { char v[16] = { 0 }; g_rec_at = GetEnvironmentVariableA("SATURN_REC", v, 16) > 0 ? now_s() + atof(v) : 1e18; }
	if (g_rec_i < 0) { if (!rgb || now_s() < g_rec_at) return; g_rec_i = 0; g_rec_w = w; g_rec_h = h; }
	if (g_rec_i >= REC_N + 2) return;
	if (g_rec_i < REC_N && rgb)
	{
		ID3D11Texture2D *&st = g_rec_st[g_rec_i % REC_RING];
		D3D11_TEXTURE2D_DESC sd; tex->GetDesc(&sd);
		if (st) { D3D11_TEXTURE2D_DESC cd; st->GetDesc(&cd); if (cd.Format != sd.Format) release(st); }
		if (!st)
		{
			D3D11_TEXTURE2D_DESC d = {};
			d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = sd.Format;
			d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			g_dev->CreateTexture2D(&d, nullptr, &st);
		}
		if (st) g_ctx->CopyResource(st, tex);
		g_rec_t[g_rec_i] = now_s();
		g_rec_kind[g_rec_i] = g_rec_kind_next;
	}
	if (g_rec_i >= 2) rec_flush_one(g_rec_i - 2);
	g_rec_i++;
	if (g_rec_i == REC_N + 2) rec_write_all();
}

// ---------- recording of what really reaches the screen: SATURN_REC_BACK=<seconds> ----------
// 960x540 crop at the center of the swapchain image (after scaling and RTX VSR, before the UI),
// 360 consecutive images with their type (real/generated): rec_back\b_NNN.raw (BGRA) + b.txt
// SATURN_REC_BACK_FULL=1: full image (4K) instead of the crop, 60 images
static int RB_N = 360, RB_W = 960, RB_H = 540;
static const int RB_RING = 4, RB_MAX = 360;
static ID3D11Texture2D *g_rb_st[RB_RING];
static uint8_t *g_rb_buf[RB_MAX];
static int g_rb_kind[RB_MAX];
static double g_rb_t[RB_MAX];
static int g_rb_i = -1;
static double g_rb_at = -1;

static void rb_flush(int i)
{
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g_ctx->Map(g_rb_st[i % RB_RING], 0, D3D11_MAP_READ, 0, &m))) return;
	g_rb_buf[i] = (uint8_t *)malloc((size_t)RB_W * RB_H * 4);
	for (int y = 0; y < RB_H; y++) memcpy(g_rb_buf[i] + (size_t)y * RB_W * 4, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)RB_W * 4);
	g_ctx->Unmap(g_rb_st[i % RB_RING], 0);
}

static void rec_back(ID3D11Texture2D *back)
{
	if (!back) return;
	if (g_rb_at < 0) { char v[16] = { 0 }; g_rb_at = GetEnvironmentVariableA("SATURN_REC_BACK", v, 16) > 0 ? now_s() + atof(v) : 1e18; }
	if (g_rb_i < 0)
	{
		if (now_s() < g_rb_at) return;
		g_rb_i = 0;
		char v[4] = { 0 };
		if (GetEnvironmentVariableA("SATURN_REC_BACK_FULL", v, 4) > 0) { D3D11_TEXTURE2D_DESC bd; back->GetDesc(&bd); RB_W = bd.Width; RB_H = bd.Height; RB_N = 60; }
	}
	if (g_rb_i >= RB_N + 2) return;
	if (g_rb_i < RB_N)
	{
		ID3D11Texture2D *&st = g_rb_st[g_rb_i % RB_RING];
		if (!st)
		{
			D3D11_TEXTURE2D_DESC d = {}; back->GetDesc(&d);
			d.Width = RB_W; d.Height = RB_H; d.BindFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			g_dev->CreateTexture2D(&d, nullptr, &st);
		}
		D3D11_TEXTURE2D_DESC bd; back->GetDesc(&bd);
		UINT x0 = (bd.Width - RB_W) / 2, y0 = (bd.Height - RB_H) / 2;
		D3D11_BOX box = { x0, y0, 0, x0 + RB_W, y0 + RB_H, 1 };
		if (st) g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, back, 0, &box);
		g_rb_kind[g_rb_i] = g_rec_kind_next; g_rb_t[g_rb_i] = now_s();
	}
	if (g_rb_i >= 2) rb_flush(g_rb_i - 2);
	if (++g_rb_i == RB_N + 2)
	{
		char dir[MAX_PATH]; snprintf(dir, sizeof(dir), "%s\\rec_back", settings_dir()); CreateDirectoryA(dir, nullptr);
		char p[MAX_PATH]; snprintf(p, sizeof(p), "%s\\b.txt", dir);
		FILE *t = fopen(p, "w");
		for (int i = 0; i < RB_N; i++)
		{
			if (t) fprintf(t, "%d %.6f %d\n", i, g_rb_t[i], g_rb_kind[i]);
			if (!g_rb_buf[i]) continue;
			snprintf(p, sizeof(p), "%s\\b_%03d.raw", dir, i);
			FILE *f = fopen(p, "wb"); if (f) { fwrite(g_rb_buf[i], 1, (size_t)RB_W * RB_H * 4, f); fclose(f); }
			free(g_rb_buf[i]); g_rb_buf[i] = nullptr;
		}
		if (t) fclose(t);
		fprintf(stderr, "Screen recording saved in %s\n", dir);
	}
}

// called with hw_lock held: resizes the swapchain if the window changed
static void apply_resize()
{
	if (!InterlockedExchange(&g_resize_pending, 0)) return;
	release(g_vp); release(g_vpe);
	ui_release_targets();
	g_swap->ResizeBuffers(0, g_out_w, g_out_h, DXGI_FORMAT_UNKNOWN,
		DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
}

static void record_present()
{
	LONG i = InterlockedIncrement(&g_pti);
	g_pt[i % PT_N] = now_s();
}

// ---------- picture tuning: sharpness (contrast adaptive, like AMD CAS), shadows, saturation ----------
static const char *k_pp = R"(
Texture2D<float4> I : register(t0);
RWTexture2D<float4> O : register(u0);
cbuffer C : register(b0) { float sharp; float shadows; float sat; float pad; uint2 size; uint2 pad2; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y) return;
	int2 p = id.xy, m = (int2)size - 1;
	float3 c = I.Load(int3(p, 0)).rgb;
	if (sharp > 0)
	{
		float3 n = I.Load(int3(clamp(p + int2(0, -1), 0, m), 0)).rgb, s = I.Load(int3(clamp(p + int2(0, 1), 0, m), 0)).rgb;
		float3 e = I.Load(int3(clamp(p + int2(1, 0), 0, m), 0)).rgb, w = I.Load(int3(clamp(p + int2(-1, 0), 0, m), 0)).rgb;
		float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
		float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));   // less sharpening where there is already contrast
		float3 wg = amp * (-1.0 / lerp(8.0, 5.0, sharp));
		c = saturate((c + (n + s + e + w) * wg) / (1.0 + 4.0 * wg));
	}
	float l = dot(c, float3(0.2126, 0.7152, 0.0722));
	if (shadows > 0)
	{
		float3 up = pow(max(c, 1e-5), 1.0 / (1.0 + shadows * 0.8));      // lifts dark tones, not highlights
		c = lerp(c, up, shadows * saturate(1.0 - l * 1.6));
		l = dot(c, float3(0.2126, 0.7152, 0.0722));
	}
	if (sat != 1.0) c = saturate(lerp(l.xxx, c, sat));
	O[p] = float4(c, 1);
}
)";
static ID3D11ComputeShader *g_pp_cs;
static ID3D11Buffer *g_pp_cb;
static ID3D11Texture2D *g_pp_out[2];
static ID3D11UnorderedAccessView *g_pp_uav[2];
static int g_pp_i, g_pp_w, g_pp_h;
static ID3D11Texture2D *g_pp_srv_tex[16];
static ID3D11ShaderResourceView *g_pp_srv[16];
static bool g_pp_dead;

static bool pp_active() { return !g_pp_dead && (g_set.sharpen > 0 || g_set.shadows > 0 || g_set.saturation != 100); }

// with hw_lock held
static ID3D11Texture2D *pp_apply(ID3D11Texture2D *src, int w, int h)
{
	if (!g_pp_cs)
	{
		ID3DBlob *code = nullptr, *errs = nullptr;
		if (FAILED(D3DCompile(k_pp, strlen(k_pp), "pp", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)))
		{ fprintf(stderr, "Picture tuning: shader not compiled: %s\n", errs ? (const char *)errs->GetBufferPointer() : "?"); release(errs); g_pp_dead = true; return nullptr; }
		g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_pp_cs); code->Release();
		D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		g_dev->CreateBuffer(&bd, nullptr, &g_pp_cb);
	}
	if (!g_pp_cs || !g_pp_cb) return nullptr;
	if (!g_pp_out[0] || w != g_pp_w || h != g_pp_h)
	{
		for (int i = 0; i < 2; i++) { release(g_pp_uav[i]); release(g_pp_out[i]); }
		D3D11_TEXTURE2D_DESC td = {}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		for (int i = 0; i < 2; i++)
			if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_pp_out[i])) || FAILED(g_dev->CreateUnorderedAccessView(g_pp_out[i], nullptr, &g_pp_uav[i]))) { g_pp_dead = true; return nullptr; }
		g_pp_w = w; g_pp_h = h;
	}
	// input image view (surfaces are few and fixed: views are cached)
	ID3D11ShaderResourceView *srv = nullptr;
	for (int i = 0; i < 16; i++) if (g_pp_srv_tex[i] == src) { srv = g_pp_srv[i]; break; }
	if (!srv)
	{
		static int next;
		int k = next++ % 16;
		release(g_pp_srv[k]);
		if (FAILED(g_dev->CreateShaderResourceView(src, nullptr, &g_pp_srv[k]))) { g_pp_srv_tex[k] = nullptr; return nullptr; }
		g_pp_srv_tex[k] = src; srv = g_pp_srv[k];
	}
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(g_ctx->Map(g_pp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
	struct { float sharp, shadows, sat, pad; UINT size[2], pad2[2]; } c = { g_set.sharpen / 100.f, g_set.shadows / 100.f, g_set.saturation / 100.f, 0, { (UINT)w, (UINT)h }, { 0, 0 } };
	memcpy(m.pData, &c, sizeof(c)); g_ctx->Unmap(g_pp_cb, 0);
	int o = g_pp_i ^= 1;
	g_ctx->CSSetShader(g_pp_cs, nullptr, 0);
	g_ctx->CSSetShaderResources(0, 1, &srv);
	g_ctx->CSSetUnorderedAccessViews(0, 1, &g_pp_uav[o], nullptr);
	g_ctx->CSSetConstantBuffers(0, 1, &g_pp_cb);
	g_ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
	ID3D11ShaderResourceView *ns = nullptr; ID3D11UnorderedAccessView *nu = nullptr;
	g_ctx->CSSetShaderResources(0, 1, &ns); g_ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
	return g_pp_out[o];
}

// ---------- screenshot ----------
static volatile double g_shot_at;
void video_screenshot() { g_shot_at = now_s() + 0.35; } // a moment later: the menu that asked for it closes first

struct Shot { uint8_t *px; UINT w, h, pitch; };
static DWORD WINAPI shot_thread(void *arg)
{
	Shot *sh = (Shot *)arg;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	PWSTR pics = nullptr; wchar_t path[MAX_PATH] = {};
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)))
	{
		time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
		swprintf(path, MAX_PATH, L"%s\\SaturnProject", pics); CreateDirectoryW(path, nullptr);
		swprintf(path, MAX_PATH, L"%s\\SaturnProject\\SaturnProject_%04d-%02d-%02d_%02d-%02d-%02d.png", pics, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
		CoTaskMemFree(pics);
	}
	IWICImagingFactory *f = nullptr; IWICStream *st = nullptr; IWICBitmapEncoder *enc = nullptr; IWICBitmapFrameEncode *fr = nullptr;
	bool ok = path[0] && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))
		&& SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE))
		&& SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache))
		&& SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(sh->w, sh->h));
	if (ok)
	{
		WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGR; fr->SetPixelFormat(&pf);
		ok = SUCCEEDED(fr->WritePixels(sh->h, sh->pitch, sh->pitch * sh->h, sh->px)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
	}
	if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (f) f->Release();
	free(sh->px); delete sh;
	ui_toast(ok ? T(L"Foto salvata in Immagini › SaturnProject", L"Screenshot saved in Pictures › SaturnProject") : T(L"Foto non salvata", L"Screenshot not saved"), 3);
	CoUninitialize();
	return 0;
}

// with hw_lock held: copies the swapchain image (game, without UI) and saves it on another thread
static void take_shot(ID3D11Texture2D *back)
{
	D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
	d.BindFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *st = nullptr;
	if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
	g_ctx->CopyResource(st, back);
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
	{
		Shot *sh = new Shot{ (uint8_t *)malloc((size_t)d.Width * 4 * d.Height), d.Width, d.Height, d.Width * 4 };
		for (UINT y = 0; y < d.Height; y++) memcpy(sh->px + (size_t)y * sh->pitch, (uint8_t *)m.pData + (size_t)y * m.RowPitch, sh->pitch);
		g_ctx->Unmap(st, 0);
		CloseHandle(CreateThread(nullptr, 0, shot_thread, sh, 0, nullptr));
	}
	st->Release();
}

// shows a surface (decoded NV12, or converted/generated RGBA) at the next display refresh
static bool g_pacer_waited; // the frame pacer already waited for the compositor for this image
static bool g_present_repeat; // this present repeats the previous image
static bool g_present_vrr;    // variable refresh: immediate Present (the monitor refreshes now)
static void present_tex(ID3D11Texture2D *tex, UINT slice, int w, int h, bool rgb = false, UINT sync = 1)
{
	if (!g_pacer_waited) WaitForSingleObject(g_waitable, 100); // the compositor is ready for another image
	g_pacer_waited = false;
	// pacing is measured here: with a full queue the wait ends at every display refresh, so the
	// intervals are the ones really seen (the return of Present instead jitters even when the display is regular)
	bool was_repeat = g_present_repeat; g_present_repeat = false;
	if (!was_repeat) record_present(); // statistics only count NEW images (not the pacer's repeats)
	hw_lock();
	apply_resize();
	ID3D11Texture2D *back = nullptr;
	g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
	if (pp_active())
	{
		// picture tuning: needs the RGB image (decoded NV12 frames are converted first)
		ID3D11Texture2D *in = rgb ? tex : convert_rgba(tex, slice, w, h);
		ID3D11Texture2D *o = in ? pp_apply(in, w, h) : nullptr;
		if (o) { tex = o; slice = 0; rgb = true; }
	}
	if (back && ensure_vp(w, h, rgb))
	{
		place_video(w, h);
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od = {}; od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		ID3D11VideoProcessorOutputView *ov = nullptr;
		g_vdev->CreateVideoProcessorOutputView(back, g_vpe, &od, &ov);
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id = {}; id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; id.Texture2D.ArraySlice = slice;
		ID3D11VideoProcessorInputView *iv = nullptr;
		g_vdev->CreateVideoProcessorInputView(tex, g_vpe, &id, &iv);
		if (ov && iv)
		{
			D3D11_VIDEO_PROCESSOR_STREAM st = {}; st.Enable = TRUE; st.pInputSurface = iv;
			g_vctx->VideoProcessorBlt(g_vp, ov, 0, 1, &st);
		}
		release(iv); release(ov);
	}
	rec_frame(tex, w, h, rgb);
	rec_back(back);
	if (back && g_shot_at > 0 && now_s() >= g_shot_at) { g_shot_at = 0; take_shot(back); }
	if (back && ui_overlay_active()) ui_draw(back, true); // menu, statistics, notices over the game
	release(back);
	if (g_present_vrr && g_tearing) g_swap->Present(0, DXGI_PRESENT_ALLOW_TEARING); // variable refresh: the monitor refreshes now
	else g_swap->Present(sync, 0); // vsync on: no tearing. sync = for how many refreshes the image stays
	g_present_vrr = false;
	hw_unlock();
	g_last_video_present = now_s();
	if (!was_repeat) InterlockedIncrement(&g_presented);
}

// UI only (no game underneath): Home, settings, connection
static void present_ui()
{
	WaitForSingleObject(g_waitable, 100);
	hw_lock();
	apply_resize();
	ID3D11Texture2D *back = nullptr;
	g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
	if (back) ui_draw(back, false);
	release(back);
	g_swap->Present(1, 0);
	hw_unlock();
}

// ---------- frame pacer ----------
// Images (real and generated) are not shown as soon as they are ready: each one gets the time it should be seen, on a
// timeline cleaned of network jitter, and at every display refresh the right one for that moment is shown.
// Showing them immediately skipped generated frames with a 62-64 fps source or a few ms of delay, and
// the display repeated images (measured: 35-46 generated/s, 15-30 repeats/s) = stuttering motion.
struct Show { int slot; int w, h; double due; int kind; };
static const int SHN = 16;
static ID3D11Texture2D *g_ring[SHN];
static int g_ring_i;
static Show g_show[SHN];
static int g_shn;
static Show g_last_show = { -1 };
static volatile LONG g_reshown; // display refreshes with the same image (counter for the statistics)
static double g_late_adj;       // extra margin if images arrive late

// copies the image (generation BGRA/RGBA) into the ring and queues it for time 'due'
static void show(ID3D11Texture2D *src, int w, int h, double due, int kind)
{
	if (!src) return;
	double now = now_s();
	if (due < now) { g_late_adj = g_late_adj + (now - due) * 0.5; if (g_late_adj > 0.012) g_late_adj = 0.012; due = now; } // late: more margin
	else g_late_adj *= 0.998;
	hw_lock();
	int slot = g_ring_i; g_ring_i = (g_ring_i + 1) % SHN;
	D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
	ID3D11Texture2D *&t = g_ring[slot];
	if (t) { D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); if (td.Width != sd.Width || td.Height != sd.Height || td.Format != sd.Format) release(t); }
	if (!t)
	{
		D3D11_TEXTURE2D_DESC d = {}; d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = sd.Format;
		d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) { hw_unlock(); return; }
	}
	g_ctx->CopyResource(t, src);
	hw_unlock();
	EnterCriticalSection(&g_lock);
	if (g_shn == SHN) { memmove(g_show, g_show + 1, sizeof(Show) * (SHN - 1)); g_shn--; }
	// images arrive in time order; if one is earlier than the last queued, it is aligned
	if (g_shn && due < g_show[g_shn - 1].due) due = g_show[g_shn - 1].due;
	g_show[g_shn++] = { slot, w, h, due, kind };
	LeaveCriticalSection(&g_lock);
}

// at every display refresh: the most recent image already due; if none is new, the last one is repeated
static bool pace_present()
{
	double now = now_s() + 0.002;
	Show pick = { -1 }; int skipped = 0;
	EnterCriticalSection(&g_lock);
	int n = 0;
	while (n < g_shn && g_show[n].due <= now) n++;
	// variable refresh: all of them are shown, one at a time (the oldest due), unless 3+ images are behind
	if (n && g_present_vrr && n < 3) n = 1;
	if (n) { pick = g_show[n - 1]; skipped = n - 1; memmove(g_show, g_show + n, sizeof(Show) * (g_shn - n)); g_shn -= n; }
	LeaveCriticalSection(&g_lock);
	if (skipped) InterlockedAdd(&g_dropped, skipped);
	bool fresh = pick.slot >= 0;
	if (!fresh) { if (g_last_show.slot < 0) return false; pick = g_last_show; InterlockedIncrement(&g_reshown); }
	g_last_show = pick;
	g_rec_kind_next = pick.kind;
	g_pacer_waited = true; g_present_repeat = !fresh;
	present_tex(g_ring[pick.slot], 0, pick.w, pick.h, true, 1);
	if (fresh && pick.kind == 0) InterlockedIncrement(&g_real_shown);
	return true;
}

static void present_frame(AVFrame *f)
{
	// without generation: the converted image goes through the pacer (immediately, no delay)
	int w = f->width & ~1, h = f->height & ~1;
	g_conv_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
	ID3D11Texture2D *rgba = convert_rgba((ID3D11Texture2D *)f->data[0], (UINT)(intptr_t)f->data[1], w, h);
	if (rgba) show(rgba, w, h, now_s(), 0);
}

static bool fruc_setup(int w, int h)
{
	if (g_fruc_dead) return false;
	if (g_fa.handle && w == g_fw && h == g_fh) return true;
	static bool loaded;
	if (!loaded && !(loaded = fruc_load())) { g_fruc_dead = true; return false; }
	fruc_destroy(g_fa);
	g_fw = w; g_fh = h;
	if (fruc_create(g_fa, g_dev, w, h)) return true;
	fprintf(stderr, "FRUC not available\n");
	fruc_destroy(g_fa);
	g_fruc_dead = true;
	return false;
}

// A real frame N after the previous one P (distance "gap"): k images on screen per N.
//  k=1: N only                    (source already at the display rate)
//  k=2: P+1/2, N                  (60 fps -> 120)
//  k=4: P+1/4, P+1/2, P+3/4, N    (30 fps -> 120)

// 0 = generation not available (the caller shows the frame as is), 1 = shown, 2 = dropped (duplicate in a 30 fps game)
static int present_with_fg(AVFrame *f, double ts, double prev_ts, bool behind, bool copy, float ch)
{
	auto *tex = (ID3D11Texture2D *)f->data[0];
	UINT slice = (UINT)(intptr_t)f->data[1];
	int w = f->width & ~1, h = f->height & ~1;
	// engine: the chosen one, then in cascade DLSS -> FSR 3 -> optical flow -> FRUC (so it works on every GPU)
	// 2 = DLSS, 3 = FSR 3 (both through D3D12, vectors from NVOF flow if present), 0 = NVOF + our shader, 1 = FRUC
	int engine = -1;
	{
		int want[5] = { g_set.fg_engine, FGE_DLSS, FGE_FSR, FGE_NVOF, FGE_FRUC };
		for (int i = 0; i < 5 && engine < 0; i++)
		{
			int e2 = want[i];
			if (e2 == FGE_DLSS && !g_dlss_dead) engine = 2;
			else if (e2 == FGE_FSR && !g_fsr_dead) engine = 3;
			else if (e2 == FGE_NVOF && !g_nvof_dead) engine = 0;
			else if (e2 == FGE_FRUC && !g_fruc_dead) engine = 1;
		}
		if (engine < 0) return false;
	}
	g_conv_fmt = engine != 1 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; // NVOF and DLSS want BGRA, FRUC RGBA
	if (engine == 1 && !fruc_setup(w, h)) return false;
	ID3D11Texture2D *rgba = convert_rgba(tex, slice, w, h);
	if (!rgba) return false;
	// 30 fps games: the PS5 sends 60 images repeating each one. If the "new, copy, new, copy" pattern is confirmed
	// copies are dropped: 30 real images at 33 ms remain and generation makes 60-120 with real motion (instead of
	// generating between two identical images). Never two drops in a row (still scene: keep showing).
	{
		static int score; static bool last_same, last_dropped;
		if (prev_ts <= 0) { score = 0; last_same = last_dropped = false; }
		bool same = ch >= 0 && ch < 0.0005f; // PS5 copy: ~0; animated 60 fps menus: 0.0015-0.003 (measured)
		if (same && !last_same) score = score < 10 ? score + 1 : 10;
		else if (!same && !last_same) score = score > 2 ? score - 2 : 0; // two new images in a row: 60 fps game
		last_same = same;
		static bool mode30; if (prev_ts <= 0) mode30 = false; if (score >= 10) mode30 = true; else if (score == 0) mode30 = false; // hysteresis: no constant 60/30 switching
		bool drop = same && mode30 && !last_dropped;
		last_dropped = drop;
		static int dbg = -1; if (dbg < 0) { char v[4] = { 0 }; dbg = GetEnvironmentVariableA("SATURN_DEBUG_DUPS", v, 4) > 0; }
		if (dbg) { static long n, nd; static double last; n++; nd += drop; if (ts - last > 1.0) { fprintf(stderr, "duplicates: %ld dropped of %ld, score %d, changed %.4f\n", nd, n, score, ch); n = nd = 0; last = ts; } }
		if (drop) { InterlockedIncrement(&g_dups); return 2; }
	}
	double period = 1.0 / g_refresh, gap = ts - prev_ts;
	// how many images per real frame: from the AVERAGE source rate, not from a single interval. At 60 fps the network
	// delivers frames at 12 and 21 ms instead of 16.7: decided one by one, the early one was left without
	// an in-between frame and the display repeated an image (measured: 51 generated of 60, 9 repeats/s). Variations are
	// absorbed by the frame pacer.
	static double ema = 0;
	if (prev_ts <= 0) ema = 0;
	else if (gap > 0.004 && gap < 0.1)
	{
		if (ema <= 0 || gap > ema * 2.5 || gap < ema * 0.4) ema = ema <= 0 ? gap : ema * 0.7 + gap * 0.3; // rate jump: adapt quickly
		else ema = ema * 0.9 + gap * 0.1;
	}
	int k = 1;
	// with in-between frames the real image P is shown only once N has arrived: one more source interval.
	// Audio is delayed by the same amount (in 10 ms steps, so it is not touched at every small change)
	if (!behind && prev_ts > 0 && ema > 0)
	{
		double n = ema / period;
		k = n < 1.5 ? 1 : n < 3.0 ? 2 : n < 5.5 ? 4 : 1;
	}
	// cleaned timeline: the new real frame falls one average interval after the previous one (if the network delivers
	// it a bit early or late it does not matter), slowly re-aligned to the arrival time; in-between frames sit at 1/4, 1/2, 3/4.
	// Display delay D: just enough for the in-between frames to be ready (they arrive with the next real frame).
	static double pts_prev;
	double pts = ts;
	if (prev_ts > 0 && pts_prev > 0 && ema > 0)
	{
		double pred = pts_prev + ema;
		pts = fabs(ts - pred) < ema * 0.5 ? pred + (ts - pred) * 0.05 : ts;
	}
	double P0 = pts_prev > 0 && prev_ts > 0 ? pts_prev : pts - (ema > 0 ? ema : 1.0 / 60);
	pts_prev = pts;
	double D = (k > 1 ? (1.0 - 1.0 / k) * (pts - P0) : 0) + 0.004 + g_late_adj;
	auto due = [&](double f) { return P0 + f * (pts - P0) + D; };
	{ LONG d2 = (LONG)(D * 100 + 0.5) * 10; if (d2 > 60) d2 = 60; if (labs(d2 - g_av_delay_ms) >= 10) InterlockedExchange(&g_av_delay_ms, d2); }
	{
		// diagnostics: SATURN_DEBUG_K=1 logs every second how many times k=1/2/4, how many late, average interval
		static int dbg = -1; static long c1, c2, c4, cb; static double last;
		if (dbg < 0) { char v[4] = { 0 }; dbg = GetEnvironmentVariableA("SATURN_DEBUG_K", v, 4) > 0; }
		if (dbg)
		{
			if (behind) cb++;
			if (k == 1) c1++; else if (k == 2) c2++; else c4++;
			if (ts - last > 1.0) { fprintf(stderr, "k: 1=%ld 2=%ld 4=%ld late=%ld average %.2f ms gap %.2f\n", c1, c2, c4, cb, ema * 1000, gap * 1000); c1 = c2 = c4 = cb = 0; last = ts; }
		}
	}

	// quality check (SATURN_OF_EVAL=1): one real frame out of two is skipped; the image halfway between the two
	// remaining ones (A, C) is compared with the skipped one (B). With SATURN_REC pairs are recorded: type 2 = generated,
	// type 7 = real reference. The screen shows 15 pairs per second: tests only.
	static int eval = -1;
	if (eval < 0) { char v[8] = { 0 }; eval = GetEnvironmentVariableA("SATURN_OF_EVAL", v, 8) > 0 ? 1 : 0; }
	if (eval && engine == 0)
	{
		static ID3D11Texture2D *truth; static int ev_i;
		hw_lock();
		bool ok = ofi_init(g_dev, g_ctx, w, h, 5);
		if (ok && !truth) { D3D11_TEXTURE2D_DESC d; rgba->GetDesc(&d); d.BindFlags = D3D11_BIND_SHADER_RESOURCE; g_dev->CreateTexture2D(&d, nullptr, &truth); }
		ID3D11Texture2D *mid = nullptr, *alt = nullptr;
		if (ok && truth)
		{
			if (ev_i++ % 2) g_ctx->CopyResource(truth, rgba);
			else if (ofi_push(rgba) && ev_i > 2) { mid = ofi_interp(0.5f); alt = ofi_interp_alt(0.5f, true); }
		}
		hw_unlock();
		// type 2 = chosen shader, 8 = the other one, 7 = real: the same pairs for both
		if (mid)
		{
			g_rec_kind_next = 2; present_tex(mid, 0, w, h, true);
			if (alt) { g_rec_kind_next = 8; present_tex(alt, 0, w, h, true); }
			g_rec_kind_next = 7; present_tex(truth, 0, w, h, true);
		}
		return true;
	}
	if (engine == 2 || engine == 3)
	{
		// DLSS/FSR Frame Generation: NVOF flow (current->previous) provides the motion vectors, the NVIDIA/AMD network
		// generates the midpoint. One in-between frame per pair: 60 -> 120; from 30 (k=4) with DLSS a cascade
		// gives 1/4, 1/2, 3/4 (120). NVOF level as below: best at 30 fps, medium at 60.
		static double last_ts, flow_ema, since;
		static int perf = 5, want = 5;
		if (last_ts > 0 && ts - last_ts < 0.1) flow_ema = flow_ema > 0 ? flow_ema * 0.9 + (ts - last_ts) * 0.1 : ts - last_ts;
		last_ts = ts;
		int w2 = flow_ema > 0 && flow_ema < 0.025 ? 10 : 5;
		if (w2 != want) { want = w2; since = ts; }
		if (want != perf && ts - since > 2.0) perf = want;
		hw_lock();
		// NVIDIA optical flow provides the vectors; without it (AMD/Intel GPUs) FSR works with zero vectors and its own flow
		bool of = !g_nvof_dead && ofi_init(g_dev, g_ctx, w, h, perf);
		if (!of && engine == 2) { hw_unlock(); g_nvof_dead = true; g_dlss_dead = true; fprintf(stderr, "NVIDIA optical flow missing: DLSS not usable\n"); return false; }
		if (!of) g_nvof_dead = true;
		if (!dlssg_init(g_dev, w, h, engine == 2 ? FG12_DLSS : FG12_FSR))
		{
			hw_unlock();
			(engine == 2 ? g_dlss_dead : g_fsr_dead) = true;
			fprintf(stderr, "%s not available: trying the next engine\n", engine == 2 ? "DLSS" : "FSR 3");
			return false;
		}
		static bool have_prev;
		if (prev_ts <= 0) have_prev = false;
		bool pair;
		if (of) { pair = ofi_push(rgba, true); if (pair) ofi_motion(dlssg_motion_uav()); }
		else
		{
			static bool cleared; if (!cleared) { float z[4] = {}; g_ctx->ClearUnorderedAccessViewFloat(dlssg_motion_uav(), z); cleared = true; }
			pair = have_prev;
		}
		have_prev = true;
		g_ctx->CopyResource(dlssg_input(), rgba);
		double ms = 0;
		// 30 fps source with DLSS: cascade, 4 images per real one (30 -> 120, each shown once)
		ID3D11Texture2D *gen = nullptr, *q1 = nullptr, *mid = nullptr, *q3 = nullptr;
		bool quad = engine == 2 && k == 4 && g_set.fg != FG_X2 && pair && dlssg_generate4(g_ctx4, false, &q1, &mid, &q3, &ms);
		if (!quad) gen = dlssg_generate(g_ctx4, !pair, gap * 1000.0, &ms);
		ID3D11Texture2D *cur = of ? ofi_current() : dlssg_current();
		hw_unlock();
		g_engine_name = dlssg_name();
		if (quad)
		{
			show(q1, w, h, due(0.25), 1); show(mid, w, h, due(0.5), 2); show(q3, w, h, due(0.75), 3); show(cur, w, h, due(1.0), 0);
			InterlockedAdd(&g_generated, 3);
			g_fruc_ms = g_fruc_ms * 0.9 + 0.1 * ms;
			return true;
		}
		UINT hold = k == 4 ? 2 : 1;
		(void)hold;
		if (gen && pair && k >= 2) { show(gen, w, h, due(0.5), 2); InterlockedIncrement(&g_generated); }
		show(cur, w, h, due(1.0), 0);
		g_fruc_ms = g_fruc_ms * 0.9 + 0.1 * ms;
		return true;
	}
	if (engine == 0)
	{
		LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
		// best optical flow quality for 30 fps sources; medium level for 60 fps ones: at best quality the
		// NVOF engine of an RTX 4080 handles ~51 pairs/s at 1080p (measured: 51 generated of 60, 9 repeats/s; at
		// medium level 60 of 60, 0 repeats). It only changes after 2 s of stable rate (the optical flow object is recreated).
		// For a frame that duplicates the previous one the flow is not computed (it would be zero): the image
		// stays for 2 refreshes. This way real computations are 30/s in 30 fps games and flow stays at best quality.
		// (If detection is wrong, a real frame taken for a copy is only held: a moment of 30 fps, never chaotic pacing.)
		bool skip_flow = g_set.fg == FG_SMOOTH && copy && k == 2;
		static double last_flow, flow_ema;
		if (!skip_flow) { if (last_flow > 0 && ts - last_flow < 0.1) flow_ema = flow_ema > 0 ? flow_ema * 0.9 + (ts - last_flow) * 0.1 : ts - last_flow; last_flow = ts; }
		static int perf = 5, want = 5; static double since;
		int w2 = flow_ema > 0 && flow_ema < 0.025 ? 10 : 5;
		if (w2 != want) { want = w2; since = ts; }
		if (want != perf && ts - since > 2.0) perf = want;
		hw_lock();
		bool ok = ofi_init(g_dev, g_ctx, w, h, perf);
		bool pair = ok && ofi_push(rgba, !skip_flow);
		ID3D11Texture2D *q[3] = {};
		int n = 0;
		// x2: with a 30 fps source only the midpoint image (30 -> 60, half real images)
		UINT hold = 1;
		if (skip_flow) {} // the copy is shown twice below
		else if (pair && k == 4 && g_set.fg == FG_X2) { q[0] = ofi_interp(0.5f); n = 1; hold = 2; }
		else if (pair && k == 4) { q[0] = ofi_interp(0.25f); q[1] = ofi_interp(0.5f); q[2] = ofi_interp(0.75f); n = 3; }
		else if (pair && k == 2) { q[0] = ofi_interp(0.5f); n = 1; }
		ID3D11Texture2D *cur = ofi_current();
		hw_unlock();
		QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
		if (!ok) { g_nvof_dead = true; fprintf(stderr, "NVOF not available: trying FRUC\n"); return false; }
		g_engine_name = "NVOF";
		for (int i = 0; i < n; i++)
			if (q[i]) { show(q[i], w, h, due(n == 3 ? 0.25 * (i + 1) : 0.5), n == 3 ? i + 1 : 2); InterlockedIncrement(&g_generated); }
		(void)hold;
		show(cur, w, h, due(1.0), 0);
		g_fruc_ms = g_fruc_ms * 0.9 + 0.1 * (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
		return true;
	}
	// FRUC: ONE pass per real frame, always increasing times. (Passing the pair again backwards to
	// get the quarters gave 1/4 ~ old and 1/2 ~ new: back-and-forth image = flicker and trails. Measured.)
	// 30 fps (k=4): midpoint + real, each held 2 refreshes -> smooth 60 fps on a 120 Hz display.
	g_engine_name = "FRUC";
	double ms = 0;
	ID3D11Texture2D *mid = nullptr;
	UINT hold = k == 4 ? 2 : 1;
	bool okm = fruc_process(g_fa, g_ctx4, hw_lock, hw_unlock, rgba, 0, w, h, ts, false, prev_ts + gap / 2, &mid, nullptr, &ms);
	if (k >= 2 && okm) { g_rec_kind_next = 2; present_tex(mid, 0, w, h, true, hold); InterlockedIncrement(&g_generated); }
	g_rec_kind_next = 0; present_tex(g_fa.last_in, 0, w, h, true, hold);
	InterlockedIncrement(&g_real_shown);
	g_fruc_ms = g_fruc_ms * 0.9 + 0.1 * ms;
	if (g_fa.failures > 30)
	{
		fprintf(stderr, "FRUC: too many errors, switched off\n");
		g_fruc_dead = true;
	}
	return true;
}

static DWORD WINAPI presenter(void *)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	if (getenv("SATURN_MMCSS")) { DWORD idx = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Games", &idx); if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH); } // Windows multimedia scheduler (optional)
	HANDLE h[2] = { g_stop_event, g_ready_event };
	double prev_ts = 0;
	LONG gen = -1;
	for (;;)
	{
		if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) break;
		if (!g_streaming || g_last_show.slot < 0)
		{
			// no game (or before the first image): UI only, waiting for decode
			DWORD r = WaitForMultipleObjects(2, h, FALSE, 16);
			if (r == WAIT_OBJECT_0) break;
			if (r == WAIT_TIMEOUT && (!g_streaming || g_last_show.slot < 0)) { present_ui(); continue; }
		}
		if (!g_streaming) { EnterCriticalSection(&g_lock); g_shn = 0; LeaveCriticalSection(&g_lock); g_last_show.slot = -1; }
		// 1) every decoded image that arrived: generation and queueing with their own time
		for (;;)
		{
			EnterCriticalSection(&g_lock);
			if (!g_qn) { LeaveCriticalSection(&g_lock); break; }
			Entry e = g_q[0];
			memmove(g_q, g_q + 1, sizeof(Entry) * (g_qn - 1));
			g_qn--;
			LeaveCriticalSection(&g_lock);
			if (gen != g_session_gen)
			{
				gen = g_session_gen; prev_ts = 0; hw_lock(); ofi_reset(); hw_unlock();
				EnterCriticalSection(&g_lock); g_shn = 0; LeaveCriticalSection(&g_lock); g_last_show.slot = -1;
			}
			bool fg = g_fg_available && g_set.fg != FG_OFF && !g_set.competitive && !(g_nvof_dead && g_fruc_dead && g_dlss_dead && g_fsr_dead);
			if (g_latency_set != 1) { g_swap->SetMaximumFrameLatency(1); g_latency_set = 1; }
			if (!fg) { g_engine_name = ""; InterlockedExchange(&g_av_delay_ms, 0); }
			int done = fg ? present_with_fg(e.f, e.ts, prev_ts, false, e.copy, e.ch) : 0;
			if (!done) present_frame(e.f);
			if (done != 2) prev_ts = e.ts; // a dropped copy does not count: the interval stays the one between real images
			static AVFrame *held[HOLD]; static int hi;
			av_frame_free(&held[hi]);
			held[hi] = e.f; hi = (hi + 1) % HOLD;
		}
		// 2) to the screen
		if (!g_streaming || g_last_show.slot < 0 && !g_shn) continue;
		if (g_set.vrr && g_tearing)
		{
			// variable refresh: present when an image is due (not earlier than 1/max refresh after the
			// previous one); without new images the last one is re-presented only for the menu (animations) or every 20 ms
			static double last_p;
			double now = now_s(), next = 1e18;
			EnterCriticalSection(&g_lock); if (g_shn) next = g_show[0].due; LeaveCriticalSection(&g_lock);
			double min_gap = 1.0 / (g_refresh > 0 ? g_refresh : 144) - 0.0003;
			bool due_now = next <= now + 0.0004 && now - last_p >= min_gap;
			bool refresh = !g_shn && now - last_p > (ui_overlay_active() ? 1.0 / 60 : 0.020);
			if (due_now || refresh)
			{
				WaitForSingleObject(g_waitable, 30);
				g_present_vrr = true;
				if (!pace_present()) { g_present_vrr = false; present_ui(); }
				last_p = now_s();
			}
			else
			{
				// short wait: wakes up for a newly decoded image or when the next one is due
				double wait = next - now < 0.004 ? next - now : 0.004;
				if (now - last_p < min_gap && min_gap - (now - last_p) > wait) wait = min_gap - (now - last_p);
				DWORD ms = wait > 0.0015 ? (DWORD)(wait * 1000 - 0.5) : 0;
				if (ms) { HANDLE hh[2] = { g_stop_event, g_ready_event }; WaitForMultipleObjects(2, hh, FALSE, ms); }
				else { for (double t1 = now_s(); now_s() - t1 < wait;) YieldProcessor(); }
			}
			continue;
		}
		WaitForSingleObject(g_waitable, 30);
		if (!pace_present()) present_ui();
	}
	return 0;
}

void video_resize(int w, int h)
{
	if (!g_swap || w <= 0 || h <= 0 || (w == g_out_w && h == g_out_h)) return;
	g_out_w = w; g_out_h = h;
	InterlockedExchange(&g_resize_pending, 1);
}

// display refreshes counted by the frame pacer: how many times the display had to repeat an image
// (= stutter)
VideoStats video_stats()
{
	DXGI_FRAME_STATISTICS fs = {};
	long vs = 0;
	if (g_swap && SUCCEEDED(g_swap->GetFrameStatistics(&fs))) vs = (long)fs.SyncRefreshCount - (long)fs.PresentCount;
	VideoStats s = {};
	s.decoded = g_decoded; s.presented = g_presented; s.dropped = g_dropped; s.errors = g_errors; s.dups = g_dups;
	s.generated = g_generated; s.real_shown = g_real_shown;
	s.decode_ms = g_decode_ms; s.fruc_ms = g_fruc_ms; s.avg_packet = g_ema_big; s.vsyncs = g_reshown; (void)vs;
	s.bytes = g_bytes_total;
	s.src_w = g_src_w; s.src_h = g_src_h; s.out_w = g_out_w; s.out_h = g_out_h;
	s.engine = g_engine_name;
	s.refresh = g_refresh;
	return s;
}

int video_av_delay_ms() { return g_streaming ? (int)g_av_delay_ms : 0; }

// intervals between the last Presents (ms), oldest first; only those in the last 'window' seconds
int video_frame_times(float *out, int max, double window)
{
	LONG last = g_pti;
	double now = now_s();
	int n = 0;
	static float tmp[PT_N];
	for (int i = 0; i < PT_N - 2 && n < max; i++)
	{
		LONG a = last - i, b = last - i - 1;
		if (b < 1) break;
		double ta = g_pt[a % PT_N], tb = g_pt[b % PT_N];
		if (now - ta > window || ta <= tb) break;
		tmp[n++] = (float)((ta - tb) * 1000.0);
	}
	for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	return n;
}

void video_fini()
{
	if (g_presenter)
	{
		SetEvent(g_stop_event);
		WaitForSingleObject(g_presenter, 3000);
		CloseHandle(g_presenter); g_presenter = nullptr;
	}
	video_end();
	fruc_destroy(g_fa);
	dlssg_fini();
	ui_fini();
	if (g_recv) av_frame_free(&g_recv);
	if (g_pkt) av_packet_free(&g_pkt);
	av_buffer_unref(&g_hwdev);
	release(g_vp); release(g_vpe); release(g_swap); release(g_vctx); release(g_vdev); release(g_ctx4); release(g_ctx); release(g_dev);
}

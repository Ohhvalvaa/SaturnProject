#include "dlssg.h"
#ifdef SATURN_NO_DLSS
bool dlssg_init(ID3D11Device *, int, int, int) { return false; }
void dlssg_fini() {}
int dlssg_width() { return 0; }
int dlssg_height() { return 0; }
ID3D11Texture2D *dlssg_input() { return nullptr; }
ID3D11UnorderedAccessView *dlssg_motion_uav() { return nullptr; }
ID3D11Texture2D *dlssg_generate(ID3D11DeviceContext4 *, bool, double, double *) { return nullptr; }
bool dlssg_generate4(ID3D11DeviceContext4 *, bool, ID3D11Texture2D **, ID3D11Texture2D **, ID3D11Texture2D **, double *) { return false; }
const char *dlssg_name() { return ""; }
ID3D11Texture2D *dlssg_current() { return nullptr; }
#else
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <vector>
#include "../../third_party/dlss/include/nvsdk_ngx.h"
#include "../../third_party/dlss/include/nvsdk_ngx_helpers_dlssg_d3d.h"
#include "../../third_party/fsr/api/include/ffx_api.h"
#include "../../third_party/fsr/api/include/ffx_api_loader.h"
#include "../../third_party/fsr/api/include/dx12/ffx_api_dx12.h"
#include "../../third_party/fsr/framegeneration/include/ffx_framegeneration.h"
#include "../../third_party/fsr/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

#define TRACE(...) do { fprintf(stderr, "DLSS: " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

extern bool settings_extract_rc(int id, const wchar_t *dir, const wchar_t *name); // framegen.cpp

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static ID3D12Device *g12;
static ID3D12CommandQueue *g_q;
static const int RING = 3;
static ID3D12CommandAllocator *g_alloc[RING];
static UINT64 g_alloc_fv[RING];
static ID3D12GraphicsCommandList *g_list;
static int g_ri;
static ID3D12Fence *g_f12;
static ID3D11Fence *g_f11;
static UINT64 g_fv;
static HANDLE g_ev;
// two alternating input images: DLSSG may still be reading the previous one
static ID3D12Resource *g_in12[2], *g_mv12, *g_out12, *g_depth12;
static ID3D11Texture2D *g_in11[2], *g_mv11, *g_out11;
static ID3D11UnorderedAccessView *g_mv_uav;
static int g_cur;
static NVSDK_NGX_Parameter *g_par;
static NVSDK_NGX_Handle *g_feat;
static bool g_ngx;
static int g_w, g_h;
static unsigned long long g_frame_id;
// second generation in cascade (30 -> 120): its own parameters, input images (ring) and two shared outputs
static NVSDK_NGX_Handle *g_feat2;
static NVSDK_NGX_Parameter *g_par2;
static unsigned long long g_frame_id2;
static ID3D12Resource *g_c2[4];
static int g_c2i;
static ID3D12Resource *g_q1_12, *g_q3_12;
static ID3D11Texture2D *g_q1_11, *g_q3_11;
static bool g_quad_dead;
static ID3D11Device *g_dev11; // to create the cascade when needed
static int g_backend = -1;
// AMD FSR 3 (AMD-signed DLLs extracted from the exe: resources 13 loader, 14 frame generation)
static ffxFunctions g_ffx;
static ffxContext g_ffx_ctx;
static uint64_t g_ffx_frame;

const char *dlssg_name() { return g_backend == FG12_FSR ? "FSR 3" : "DLSS"; }
ID3D11Texture2D *dlssg_current() { return g_in11[g_cur]; }

static bool fsr_create(const wchar_t *dir, int w, int h)
{
	if (!settings_extract_rc(13, dir, L"amd_fidelityfx_loader_dx12.dll") || !settings_extract_rc(14, dir, L"amd_fidelityfx_framegeneration_dx12.dll"))
	{ TRACE("AMD DLLs not extracted"); return false; }
	wchar_t path[MAX_PATH];
	swprintf(path, MAX_PATH, L"%s\\amd_fidelityfx_framegeneration_dx12.dll", dir); LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	swprintf(path, MAX_PATH, L"%s\\amd_fidelityfx_loader_dx12.dll", dir);
	HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m) { TRACE("AMD loader not loaded (%lu)", GetLastError()); return false; }
	ffxLoadFunctions(&g_ffx, m);
	if (!g_ffx.CreateContext || !g_ffx.Dispatch || !g_ffx.Configure || !g_ffx.DestroyContext) { TRACE("AMD API missing"); return false; }
	ffxCreateBackendDX12Desc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = g12;
	ffxCreateContextDescFrameGeneration desc = {};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	desc.header.pNext = &backend.header;
	desc.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
	desc.displaySize = { (uint32_t)w, (uint32_t)h }; desc.maxRenderSize = { (uint32_t)w, (uint32_t)h };
	desc.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
	ffxReturnCode_t r = g_ffx.CreateContext(&g_ffx_ctx, &desc.header, nullptr);
	if (r != FFX_API_RETURN_OK || !g_ffx_ctx) { TRACE("FSR context not created (%u)", (unsigned)r); g_ffx_ctx = nullptr; return false; }
	ffxQueryGetProviderVersion ver = {}; ver.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	if (g_ffx.Query && g_ffx.Query(&g_ffx_ctx, &ver.header) == FFX_API_RETURN_OK) TRACE("FSR %s", ver.versionName ? ver.versionName : "?");
	g_ffx_frame = 0;
	return true;
}

// FSR: prepare (vectors and depth) and generate in the same list; surfaces in COMMON, depth in compute-read state
static bool fsr_record(bool reset, double elapsed_ms, ID3D12Resource *color)
{
	ffxConfigureDescFrameGeneration cfg = {};
	cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
	cfg.frameGenerationEnabled = true; cfg.frameID = g_ffx_frame;
	cfg.generationRect = { 0, 0, g_w, g_h };
	if (g_ffx.Configure(&g_ffx_ctx, &cfg.header) != FFX_API_RETURN_OK) return false;
	ffxDispatchDescFrameGenerationPrepareV2 prep = {};
	prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
	prep.commandList = g_list; prep.frameID = g_ffx_frame;
	prep.renderSize = { (uint32_t)g_w, (uint32_t)g_h };
	prep.motionVectorScale = { 1, 1 };                       // vectors already in pixels, current -> previous
	prep.frameTimeDelta = elapsed_ms > 0 && elapsed_ms < 200 ? (float)elapsed_ms : 16.6667f;
	prep.reset = reset; prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f;
	prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
	prep.cameraUp[1] = prep.cameraRight[0] = prep.cameraForward[2] = 1.0f; // simulated static camera
	prep.depth = ffxApiGetResourceDX12(g_depth12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prep.motionVectors = ffxApiGetResourceDX12(g_mv12, FFX_API_RESOURCE_STATE_COMMON);
	if (g_ffx.Dispatch(&g_ffx_ctx, &prep.header) != FFX_API_RETURN_OK) return false;
	ffxDispatchDescFrameGeneration gen = {};
	gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
	gen.commandList = g_list; gen.frameID = g_ffx_frame; gen.reset = reset;
	gen.presentColor = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMMON);
	gen.outputs[0] = ffxApiGetResourceDX12(g_out12, FFX_API_RESOURCE_STATE_COMMON, FFX_API_RESOURCE_USAGE_UAV);
	gen.numGeneratedFrames = 1; gen.generationRect = cfg.generationRect;
	gen.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	gen.minMaxLuminance[0] = 0; gen.minMaxLuminance[1] = 1000;
	if (g_ffx.Dispatch(&g_ffx_ctx, &gen.header) != FFX_API_RETURN_OK) return false;
	g_ffx_frame++;
	return true;
}


int dlssg_width() { return g_w; }
int dlssg_height() { return g_h; }
ID3D11Texture2D *dlssg_input() { return g_in11[g_cur ^ 1]; } // the next one to fill
ID3D11UnorderedAccessView *dlssg_motion_uav() { return g_mv_uav; }

static ID3D12Resource *make_tex(DXGI_FORMAT f, bool uav, bool shared) // only for the depth (not shared)
{
	D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = g_w; d.Height = g_h; d.DepthOrArraySize = 1; d.MipLevels = 1;
	d.Format = f; d.SampleDesc.Count = 1; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource *r = nullptr;
	HRESULT hr = g12->CreateCommittedResource(&hp, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
	if (FAILED(hr)) { TRACE("D3D12 surface format %d: 0x%08lx", (int)f, (unsigned long)hr); return nullptr; }
	return r;
}

// shared surface: created in D3D11 (NT handle), opened in D3D12. The other way round (D3D12 -> D3D11) opening
// fails with E_INVALIDARG on an RTX 4080.
static bool make_shared(ID3D11Device *dev11, DXGI_FORMAT f, UINT bind, ID3D11Texture2D **t11, ID3D12Resource **t12)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = g_w; d.Height = g_h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
	d.BindFlags = bind; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	HRESULT hr = dev11->CreateTexture2D(&d, nullptr, t11);
	if (FAILED(hr)) { TRACE("D3D11 surface format %d: 0x%08lx", (int)f, (unsigned long)hr); return false; }
	IDXGIResource1 *xr = nullptr; HANDLE h = nullptr;
	(*t11)->QueryInterface(__uuidof(IDXGIResource1), (void **)&xr);
	if (xr) { hr = xr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h); xr->Release(); }
	if (!h) { TRACE("shared handle: 0x%08lx", (unsigned long)hr); return false; }
	hr = g12->OpenSharedHandle(h, IID_PPV_ARGS(t12));
	CloseHandle(h);
	if (FAILED(hr)) { TRACE("opening in D3D12: 0x%08lx", (unsigned long)hr); return false; }
	return true;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, std::vector<D3D12_RESOURCE_BARRIER> &v)
{
	D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
	v.push_back(br);
}

// takes a free allocator from the ring and reopens the list
static bool begin_list()
{
	g_ri = (g_ri + 1) % RING;
	if (g_f12->GetCompletedValue() < g_alloc_fv[g_ri]) { g_f12->SetEventOnCompletion(g_alloc_fv[g_ri], g_ev); WaitForSingleObject(g_ev, 1000); }
	return SUCCEEDED(g_alloc[g_ri]->Reset()) && SUCCEEDED(g_list->Reset(g_alloc[g_ri], nullptr));
}

static void submit_and_signal()
{
	g_list->Close();
	ID3D12CommandList *l = g_list; g_q->ExecuteCommandLists(1, &l);
	g_q->Signal(g_f12, ++g_fv);
	g_alloc_fv[g_ri] = g_fv;
}

static void wait_cpu()
{
	if (g_f12->GetCompletedValue() < g_fv) { g_f12->SetEventOnCompletion(g_fv, g_ev); WaitForSingleObject(g_ev, 2000); }
}

static bool fill_depth()
{
	// constant depth 0.9 (video image: no real depth), uploaded once
	D3D12_RESOURCE_DESC d = g_depth12->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total;
	g12->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
	D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
	D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
	bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *up = nullptr;
	if (FAILED(g12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) return false;
	uint8_t *p = nullptr; up->Map(0, nullptr, (void **)&p);
	for (int y = 0; y < g_h; y++) { float *row = (float *)(p + fp.Offset + (size_t)y * fp.Footprint.RowPitch); for (int x = 0; x < g_w; x++) row[x] = 0.9f; }
	up->Unmap(0, nullptr);
	if (!begin_list()) { up->Release(); return false; }
	std::vector<D3D12_RESOURCE_BARRIER> b;
	barrier(g_depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST, b);
	g_list->ResourceBarrier((UINT)b.size(), b.data());
	D3D12_TEXTURE_COPY_LOCATION dst = { g_depth12, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
	g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	b.clear(); barrier(g_depth12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, b);
	g_list->ResourceBarrier((UINT)b.size(), b.data());
	submit_and_signal(); wait_cpu();
	up->Release();
	return true;
}

static void fill_opt(NVSDK_NGX_DLSSG_Opt_Eval_Params &op, bool reset, float mscale = 1.f)
{
	op = {};
	op.multiFrameCount = 1; op.multiFrameIndex = 1;
	for (int i = 0; i < 4; i++) op.cameraViewToClip[i][i] = op.clipToCameraView[i][i] = op.clipToLensClip[i][i] = op.clipToPrevClip[i][i] = op.prevClipToClip[i][i] = 1;
	// current->previous vectors in pixels; DLSSG wants them normalized and negated (verified on exact translations)
	op.mvecScale[0] = -mscale / g_w; op.mvecScale[1] = -mscale / g_h;
	// simulated static camera: all motion is in the vectors, but projection and inverse must be consistent
	op.cameraNear = 0.1f; op.cameraFar = 1000; op.cameraFOV = 1.5707963f; op.cameraAspectRatio = (float)g_w / g_h;
	op.cameraUp[1] = 1; op.cameraRight[0] = 1; op.cameraFwd[2] = 1;
	float px = 1.0f / op.cameraAspectRatio, pz = op.cameraFar / (op.cameraFar - op.cameraNear), pw = -op.cameraNear * pz;
	op.cameraViewToClip[0][0] = px; op.cameraViewToClip[2][2] = pz; op.cameraViewToClip[2][3] = 1; op.cameraViewToClip[3][2] = pw; op.cameraViewToClip[3][3] = 0;
	op.clipToCameraView[0][0] = 1 / px; op.clipToCameraView[2][2] = 0; op.clipToCameraView[2][3] = 1 / pw; op.clipToCameraView[3][2] = 1; op.clipToCameraView[3][3] = -pz / pw;
	op.cameraMotionIncluded = true; op.reset = reset;
	op.motionVectorsInvalidValue = 3.402823466e38f;
	op.backbufferSubrectSize = { (unsigned)g_w, (unsigned)g_h }; op.outputInterpSubrectSize = { (unsigned)g_w, (unsigned)g_h };
	op.mvecsSubrectSize = { (unsigned)g_w, (unsigned)g_h }; op.depthSubrectSize = { (unsigned)g_w, (unsigned)g_h };
}

bool dlssg_init(ID3D11Device *dev11, int w, int h, int backend)
{
	if ((g_feat || g_ffx_ctx) && w == g_w && h == g_h && backend == g_backend) return true;
	dlssg_fini();
	g_w = w; g_h = h; g_backend = backend; g_dev11 = dev11;
	wchar_t dir[MAX_PATH];
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return false;
	wcscat(dir, L"\\SaturnProject"); CreateDirectoryW(dir, nullptr);
	wcscat(dir, L"\\nvidia"); CreateDirectoryW(dir, nullptr);

	// same adapter as the D3D11 device
	IDXGIDevice *xd = nullptr; IDXGIAdapter *xa = nullptr; DXGI_ADAPTER_DESC ad = {};
	dev11->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd);
	if (xd) { xd->GetAdapter(&xa); xd->Release(); }
	if (xa) { xa->GetDesc(&ad); xa->Release(); }
	IDXGIFactory4 *fac = nullptr; IDXGIAdapter1 *a12 = nullptr;
	if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { fac->EnumAdapterByLuid(ad.AdapterLuid, IID_PPV_ARGS(&a12)); fac->Release(); }
	HRESULT hr = D3D12CreateDevice(a12, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g12));
	release(a12);
	if (FAILED(hr)) { TRACE("D3D12 device not created (0x%08lx)", (unsigned long)hr); return false; }
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
	if (FAILED(g12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q)))) return false;
	for (auto &a : g_alloc) if (FAILED(g12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
	if (FAILED(g12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list)))) return false;
	g_list->Close();
	if (FAILED(g12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_f12)))) return false;
	g_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	{
		HANDLE fh = nullptr; ID3D11Device5 *d5 = nullptr;
		g12->CreateSharedHandle(g_f12, nullptr, GENERIC_ALL, nullptr, &fh);
		if (fh && SUCCEEDED(dev11->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5))) { d5->OpenSharedFence(fh, __uuidof(ID3D11Fence), (void **)&g_f11); d5->Release(); }
		if (fh) CloseHandle(fh);
		if (!g_f11) { TRACE("shared fence not opened in D3D11"); return false; }
	}

	// shared surfaces: created in D3D11, opened in D3D12
	for (int i = 0; i < 2; i++) if (!make_shared(dev11, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &g_in11[i], &g_in12[i])) { TRACE("shared image"); return false; }
	if (!make_shared(dev11, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_mv11, &g_mv12)) { TRACE("shared vectors"); return false; }
	if (!make_shared(dev11, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_out11, &g_out12)) { TRACE("shared output"); return false; }
	if (!(g_depth12 = make_tex(DXGI_FORMAT_R32_FLOAT, false, false)) || !fill_depth()) { TRACE("depth"); return false; }
	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {}; ud.Format = DXGI_FORMAT_R16G16_FLOAT; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	if (FAILED(dev11->CreateUnorderedAccessView(g_mv11, &ud, &g_mv_uav))) { TRACE("vector UAV"); return false; }

	g_frame_id = 0; g_cur = 0;
	if (backend == FG12_FSR)
	{
		// depth stays in compute-read state; FSR reads color and vectors in COMMON
		if (!fsr_create(dir, w, h)) return false;
		TRACE("FSR 3 Frame Generation ready, %dx%d", w, h);
		return true;
	}
	if (!settings_extract_rc(12, dir, L"nvngx_dlssg.dll")) { TRACE("nvngx_dlssg.dll not extracted"); return false; }
	const wchar_t *paths[] = { dir };
	NVSDK_NGX_FeatureCommonInfo common = {}; common.PathListInfo.Path = paths; common.PathListInfo.Length = 1;
	NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("5a7a0b1e-7c3d-4f5e-9a1b-53a7ce0f5a17", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.6", dir, g12, &common, NVSDK_NGX_Version_API);
	if (NVSDK_NGX_FAILED(r)) { TRACE("NGX not initialized (0x%08x)", r); return false; }
	g_ngx = true;
	NVSDK_NGX_Parameter *caps = nullptr; int avail = 0;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) || !caps) { TRACE("capabilities not read"); return false; }
	caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
	NVSDK_NGX_D3D12_DestroyParameters(caps);
	if (!avail) { TRACE("Frame Generation not available on this GPU/driver"); return false; }

	NVSDK_NGX_D3D12_AllocateParameters(&g_par);
	g_par->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, (unsigned)(NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth));
	g_par->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, (unsigned)(NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField));
	NVSDK_NGX_DLSSG_Create_Params cp = { (unsigned)w, (unsigned)h, (unsigned)DXGI_FORMAT_B8G8R8A8_UNORM, (unsigned)w, (unsigned)h, false };
	if (!begin_list()) return false;
	r = NGX_D3D12_CREATE_DLSSG(g_list, 1, 1, &g_feat, g_par, &cp);
	submit_and_signal(); wait_cpu();
	if (NVSDK_NGX_FAILED(r) || !g_feat) { TRACE("DLSSG not created (0x%08x)", r); g_feat = nullptr; return false; }
	TRACE("Frame Generation ready, %dx%d", w, h);
	return true;
}

ID3D11Texture2D *dlssg_generate(ID3D11DeviceContext4 *ctx, bool reset, double elapsed_ms, double *gpu_ms)
{
	if (!g_feat && !g_ffx_ctx) return nullptr;
	LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
	g_cur ^= 1; // the input just filled becomes the current one
	// D3D11 wrote image and vectors: the signal makes them visible to the D3D12 queue
	ctx->Signal(g_f11, ++g_fv);
	ctx->Flush();
	UINT64 ready = g_fv;
	if (!begin_list()) return nullptr;
	if (g_backend == FG12_FSR)
	{
		bool ok = fsr_record(reset || g_frame_id++ == 0, elapsed_ms, g_in12[g_cur]);
		g_list->Close();
		g_q->Wait(g_f12, ready);
		ID3D12CommandList *l = g_list; g_q->ExecuteCommandLists(1, &l);
		g_q->Signal(g_f12, ++g_fv);
		g_alloc_fv[g_ri] = g_fv;
		ctx->Wait(g_f11, g_fv);
		QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
		if (gpu_ms) *gpu_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
		if (!ok) { static int told; if (told++ < 3) TRACE("FSR: generation failed"); return nullptr; }
		return g_out11;
	}
	std::vector<D3D12_RESOURCE_BARRIER> b;
	barrier(g_in12[g_cur], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, b);
	barrier(g_mv12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, b);
	barrier(g_out12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, b);
	g_list->ResourceBarrier((UINT)b.size(), b.data());
	NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
	ep.pBackbuffer = g_in12[g_cur]; ep.pDepth = g_depth12; ep.pMVecs = g_mv12; ep.pOutputInterpFrame = g_out12;
	NVSDK_NGX_DLSSG_Opt_Eval_Params op; fill_opt(op, reset || g_frame_id == 0);
	g_par->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++g_frame_id);
	NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(g_list, g_feat, g_par, &ep, &op);
	// back to COMMON: the surfaces return to D3D11
	for (auto &x : b) { auto s = x.Transition.StateBefore; x.Transition.StateBefore = x.Transition.StateAfter; x.Transition.StateAfter = s; }
	g_list->ResourceBarrier((UINT)b.size(), b.data());
	g_list->Close();
	g_q->Wait(g_f12, ready);
	ID3D12CommandList *l = g_list; g_q->ExecuteCommandLists(1, &l);
	g_q->Signal(g_f12, ++g_fv);
	g_alloc_fv[g_ri] = g_fv;
	// the D3D11 context uses the generated image only after DLSSG is done (GPU-side wait, not CPU)
	ctx->Wait(g_f11, g_fv);
	QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
	if (gpu_ms) *gpu_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
	if (NVSDK_NGX_FAILED(r))
	{
		static int told; if (told++ < 3) TRACE("evaluation failed (0x%08x)", r);
		return nullptr;
	}
	return g_out11;
}

// creates the second generation and its surfaces the first time it is needed
static bool quad_setup(ID3D11Device *dev11)
{
	if (g_feat2) return true;
	if (g_quad_dead || !g_feat) return false;
	D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = g_w; rd.Height = g_h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count = 1;
	for (auto &c : g_c2) if (FAILED(g12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&c)))) { g_quad_dead = true; return false; }
	if (!make_shared(dev11, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_q1_11, &g_q1_12)
		|| !make_shared(dev11, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_q3_11, &g_q3_12)) { g_quad_dead = true; return false; }
	NVSDK_NGX_D3D12_AllocateParameters(&g_par2);
	g_par2->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, (unsigned)(NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth));
	g_par2->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, (unsigned)(NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField));
	NVSDK_NGX_DLSSG_Create_Params cp = { (unsigned)g_w, (unsigned)g_h, (unsigned)DXGI_FORMAT_B8G8R8A8_UNORM, (unsigned)g_w, (unsigned)g_h, false };
	if (!begin_list()) return false;
	NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSG(g_list, 1, 1, &g_feat2, g_par2, &cp);
	submit_and_signal(); wait_cpu();
	if (NVSDK_NGX_FAILED(r) || !g_feat2) { TRACE("second generation not created (0x%08x): staying at 60", r); g_feat2 = nullptr; g_quad_dead = true; return false; }
	g_frame_id2 = 0;
	TRACE("Cascaded generation ready (30 -> 120)");
	return true;
}


bool dlssg_generate4(ID3D11DeviceContext4 *ctx, bool reset, ID3D11Texture2D **q1, ID3D11Texture2D **mid, ID3D11Texture2D **q3, double *gpu_ms)
{
	if (g_backend != FG12_DLSS || !g_feat || !quad_setup(g_dev11)) return false;
	LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
	g_cur ^= 1;
	ctx->Signal(g_f11, ++g_fv);
	ctx->Flush();
	UINT64 ready = g_fv;
	if (!begin_list()) return false;
	ID3D12Resource *in = g_in12[g_cur];
	ID3D12Resource *ca = g_c2[g_c2i], *cb = g_c2[(g_c2i + 1) & 3];
	g_c2i = (g_c2i + 2) & 3; // the two previous ones stay untouched (generation may still read them)
	const auto C = D3D12_RESOURCE_STATE_COMMON, NP = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	const auto CS = D3D12_RESOURCE_STATE_COPY_SOURCE, CD = D3D12_RESOURCE_STATE_COPY_DEST;
	std::vector<D3D12_RESOURCE_BARRIER> b;
	auto flush = [&]() { if (!b.empty()) { g_list->ResourceBarrier((UINT)b.size(), b.data()); b.clear(); } };
	bool first = reset || g_frame_id == 0;
	// the second generation resets if it was not used last time (switch from 60 to 30)
	static unsigned long long last_used; bool first2 = first || last_used != g_frame_id; last_used = g_frame_id + 1;
	// 1) midpoint between the previous and the current image
	barrier(in, C, NP, b); barrier(g_mv12, C, NP, b); barrier(g_out12, C, UA, b); flush();
	NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
	ep.pBackbuffer = in; ep.pDepth = g_depth12; ep.pMVecs = g_mv12; ep.pOutputInterpFrame = g_out12;
	NVSDK_NGX_DLSSG_Opt_Eval_Params op; fill_opt(op, first);
	g_par->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++g_frame_id);
	NVSDK_NGX_Result r1 = NGX_D3D12_EVALUATE_DLSSG(g_list, g_feat, g_par, &ep, &op);
	// 2) the second generation receives the midpoint...: out comes 1/4 (between previous and midpoint); halved vectors
	barrier(g_out12, UA, CS, b); barrier(ca, C, CD, b); flush();
	g_list->CopyResource(ca, g_out12);
	barrier(ca, CD, NP, b); barrier(g_q1_12, C, UA, b); flush();
	ep.pBackbuffer = ca; ep.pOutputInterpFrame = g_q1_12;
	fill_opt(op, first2, 0.5f);
	g_par2->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++g_frame_id2);
	NVSDK_NGX_Result r2 = NGX_D3D12_EVALUATE_DLSSG(g_list, g_feat2, g_par2, &ep, &op);
	// 3) ...then the current one: out comes 3/4 (between midpoint and current)
	barrier(in, NP, CS, b); barrier(cb, C, CD, b); flush();
	g_list->CopyResource(cb, in);
	barrier(cb, CD, NP, b); barrier(g_q3_12, C, UA, b); flush();
	ep.pBackbuffer = cb; ep.pOutputInterpFrame = g_q3_12;
	fill_opt(op, false, 0.5f);
	g_par2->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++g_frame_id2);
	NVSDK_NGX_Result r3 = NGX_D3D12_EVALUATE_DLSSG(g_list, g_feat2, g_par2, &ep, &op);
	// everything back to COMMON for D3D11
	barrier(in, CS, C, b); barrier(g_mv12, NP, C, b); barrier(g_out12, CS, C, b); barrier(ca, NP, C, b); barrier(cb, NP, C, b);
	barrier(g_q1_12, UA, C, b); barrier(g_q3_12, UA, C, b); flush();
	g_list->Close();
	g_q->Wait(g_f12, ready);
	ID3D12CommandList *l = g_list; g_q->ExecuteCommandLists(1, &l);
	g_q->Signal(g_f12, ++g_fv);
	g_alloc_fv[g_ri] = g_fv;
	ctx->Wait(g_f11, g_fv);
	QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
	if (gpu_ms) *gpu_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
	if (NVSDK_NGX_FAILED(r1) || NVSDK_NGX_FAILED(r2) || NVSDK_NGX_FAILED(r3))
	{
		static int told; if (told++ < 3) TRACE("cascade failed (0x%08x 0x%08x 0x%08x)", r1, r2, r3);
		return false;
	}
	*q1 = g_q1_11; *mid = g_out11; *q3 = g_q3_11;
	return true;
}

void dlssg_fini()
{
	if (g_f12 && g_q) { g_q->Signal(g_f12, ++g_fv); wait_cpu(); }
	if (g_ffx_ctx) { g_ffx.DestroyContext(&g_ffx_ctx, nullptr); g_ffx_ctx = nullptr; }
	if (g_feat2) { NVSDK_NGX_D3D12_ReleaseFeature(g_feat2); g_feat2 = nullptr; }
	if (g_par2) { NVSDK_NGX_D3D12_DestroyParameters(g_par2); g_par2 = nullptr; }
	for (auto &c : g_c2) release(c);
	release(g_q1_11); release(g_q1_12); release(g_q3_11); release(g_q3_12);
	g_quad_dead = false;
	if (g_feat) { NVSDK_NGX_D3D12_ReleaseFeature(g_feat); g_feat = nullptr; }
	if (g_par) { NVSDK_NGX_D3D12_DestroyParameters(g_par); g_par = nullptr; }
	if (g_ngx && g12) { NVSDK_NGX_D3D12_Shutdown1(g12); g_ngx = false; }
	release(g_mv_uav);
	for (int i = 0; i < 2; i++) { release(g_in11[i]); release(g_in12[i]); }
	release(g_mv11); release(g_mv12); release(g_out11); release(g_out12); release(g_depth12);
	release(g_f11); release(g_f12);
	release(g_list); for (auto &a : g_alloc) release(a);
	release(g_q); release(g12);
	if (g_ev) { CloseHandle(g_ev); g_ev = nullptr; }
	g_fv = 0; for (auto &v : g_alloc_fv) v = 0;
	g_backend = -1;
}
#endif // SATURN_NO_DLSS

// DLSS Frame Generation test (NGX DLSSG, D3D12) outside the app.
// Two synthetic images: B = A shifted by +SHIFT pixels in x. With the right vectors the in-between image is A shifted by
// SHIFT/2. Tries both signs of the vector scale and reports which one gives the right shift.
// Usage: dlssg_probe.exe <folder containing nvngx_dlssg.dll>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <string>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"

#define CHECK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAILED %s = 0x%08lx (line %d)\n", #x, (unsigned long)_h, __LINE__); exit(1); } } while (0)

static const int W = 1920, H = 1080, SHIFT = 16;
static ID3D12Device *dev;
static ID3D12CommandQueue *q;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fv;
static HANDLE ev;

static void run_and_wait()
{
	CHECK(list->Close());
	ID3D12CommandList *l = list; q->ExecuteCommandLists(1, &l);
	q->Signal(fence, ++fv);
	fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE);
	CHECK(alloc->Reset()); CHECK(list->Reset(alloc, nullptr));
}

static ID3D12Resource *tex(DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES st)
{
	D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H; d.DepthOrArraySize = 1; d.MipLevels = 1;
	d.Format = f; d.SampleDesc.Count = 1; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource *r = nullptr; CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)));
	return r;
}

static ID3D12Resource *buf(D3D12_HEAP_TYPE t, UINT64 size)
{
	D3D12_HEAP_PROPERTIES hp = { t };
	D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
	d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *r = nullptr;
	CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)));
	return r;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
	D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
	list->ResourceBarrier(1, &br);
}

// uploads data (rows of W*bpp bytes) into a texture in state 'st'
static void upload(ID3D12Resource *t, const void *data, int bpp, D3D12_RESOURCE_STATES st)
{
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total;
	D3D12_RESOURCE_DESC d = t->GetDesc();
	dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
	ID3D12Resource *u = buf(D3D12_HEAP_TYPE_UPLOAD, total);
	uint8_t *p; CHECK(u->Map(0, nullptr, (void **)&p));
	for (int y = 0; y < H; y++) memcpy(p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (const uint8_t *)data + (size_t)y * W * bpp, (size_t)W * bpp);
	u->Unmap(0, nullptr);
	barrier(t, st, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dst = { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src = { u, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, st);
	run_and_wait();
	u->Release();
}

static std::vector<uint8_t> readback(ID3D12Resource *t, D3D12_RESOURCE_STATES st)
{
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total;
	D3D12_RESOURCE_DESC d = t->GetDesc();
	dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
	ID3D12Resource *rb = buf(D3D12_HEAP_TYPE_READBACK, total);
	barrier(t, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_TEXTURE_COPY_LOCATION src = { t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = fp;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	barrier(t, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
	run_and_wait();
	std::vector<uint8_t> out((size_t)W * H * 4);
	uint8_t *p; CHECK(rb->Map(0, nullptr, (void **)&p));
	for (int y = 0; y < H; y++) memcpy(out.data() + (size_t)y * W * 4, p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (size_t)W * 4);
	rb->Unmap(0, nullptr); rb->Release();
	return out;
}

static uint8_t pattern(int x, int y)
{
	// soft blobs + sharp squares: correlation finds the shift without ambiguity
	double v = 128 + 50 * sin(x * 0.031 + y * 0.017) * cos(y * 0.043 - x * 0.007);
	unsigned h = (unsigned)((x >> 4) * 73856093u ^ (y >> 4) * 19349663u);
	h = h * 2654435761u;
	v += ((h >> 24) & 63) - 32;
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint16_t f2h(float f) // float -> half
{
	uint32_t x; memcpy(&x, &f, 4);
	uint32_t s = (x >> 16) & 0x8000; int e = ((x >> 23) & 0xff) - 127 + 15; uint32_t m = (x >> 13) & 0x3ff;
	if (f == 0) return (uint16_t)s;
	if (e <= 0) return (uint16_t)s; if (e >= 31) return (uint16_t)(s | 0x7c00);
	return (uint16_t)(s | (e << 10) | m);
}

// horizontal shift of 'img' against image A (correlation over the whole image, -24..24 px)
static int measure_shift(const std::vector<uint8_t> &img, double *err_out)
{
	int best = 0; double be = 1e30;
	for (int s = -24; s <= 24; s++)
	{
		double e = 0; int n = 0;
		for (int y = 100; y < H - 100; y += 3)
			for (int x = 100; x < W - 100; x += 3)
			{
				int g = img[((size_t)y * W + x) * 4 + 1];
				int a = pattern(x - s, y);
				e += (double)(g - a) * (g - a); n++;
			}
		e /= n;
		if (e < be) { be = e; best = s; }
	}
	if (err_out) *err_out = sqrt(be);
	return best;
}

int main(int argc, char **argv)
{
	wchar_t dir[MAX_PATH];
	if (argc > 1) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH);
	else { GetModuleFileNameW(nullptr, dir, MAX_PATH); *wcsrchr(dir, L'\\') = 0; }

	IDXGIFactory6 *fac; CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
	IDXGIAdapter1 *ad; CHECK(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)));
	DXGI_ADAPTER_DESC1 ades; ad->GetDesc1(&ades); wprintf(L"GPU: %s\n", ades.Description);
	CHECK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
	CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
	CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
	CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);

	wchar_t data[MAX_PATH]; GetTempPathW(MAX_PATH, data);
	const wchar_t *paths[] = { dir };
	NVSDK_NGX_FeatureCommonInfo common = {}; common.PathListInfo.Path = paths; common.PathListInfo.Length = 1;
	NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("5a7a0b1e-7c3d-4f5e-9a1b-53a7ce0f5a17", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.6", data, dev, &common, NVSDK_NGX_Version_API);
	printf("NGX init: 0x%08x\n", r);
	if (NVSDK_NGX_FAILED(r)) return 1;
	NVSDK_NGX_Parameter *caps = nullptr;
	NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
	int avail = 0, needdrv = 0; unsigned mfmax = 0;
	caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
	caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needdrv);
	caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfmax);
	printf("DLSSG available: %d, driver update needed: %d, MultiFrameCountMax: %u\n", avail, needdrv, mfmax);
	if (!avail) return 1;

	std::vector<uint8_t> A((size_t)W * H * 4), B((size_t)W * H * 4);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
	{
		uint8_t a = pattern(x, y), b = pattern(x - SHIFT, y);
		uint8_t *pa = &A[((size_t)y * W + x) * 4], *pb = &B[((size_t)y * W + x) * 4];
		pa[0] = pa[1] = pa[2] = a; pa[3] = 255; pb[0] = pb[1] = pb[2] = b; pb[3] = 255;
	}
	std::vector<uint16_t> mv0((size_t)W * H * 2, 0), mvB((size_t)W * H * 2);
	for (size_t i = 0; i < (size_t)W * H; i++) { mvB[i * 2] = f2h(-(float)SHIFT); mvB[i * 2 + 1] = 0; } // B -> A: -SHIFT px in x
	std::vector<float> depth((size_t)W * H, 0.9f);

	const D3D12_RESOURCE_STATES ST_IN = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	ID3D12Resource *back = tex(DXGI_FORMAT_B8G8R8A8_UNORM, false, ST_IN);
	ID3D12Resource *mvec = tex(DXGI_FORMAT_R16G16_FLOAT, false, ST_IN);
	ID3D12Resource *dep = tex(DXGI_FORMAT_R32_FLOAT, false, ST_IN);
	ID3D12Resource *out = tex(DXGI_FORMAT_B8G8R8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	upload(dep, depth.data(), 4, ST_IN);

	for (int sign = -1; sign <= 1; sign += 2)
	{
		NVSDK_NGX_Parameter *p = nullptr; NVSDK_NGX_D3D12_AllocateParameters(&p);
		p->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, (unsigned)(NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth));
		p->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, (unsigned)(NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField));
		NVSDK_NGX_DLSSG_Create_Params cp = { W, H, (unsigned)DXGI_FORMAT_B8G8R8A8_UNORM, W, H, false };
		NVSDK_NGX_Handle *h = nullptr;
		r = NGX_D3D12_CREATE_DLSSG(list, 1, 1, &h, p, &cp);
		printf("Create DLSSG: 0x%08x\n", r);
		if (NVSDK_NGX_FAILED(r)) return 1;
		run_and_wait();
		LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq);
		for (int f = 0; f < 2; f++)
		{
			upload(back, f == 0 ? A.data() : B.data(), 4, ST_IN);
			upload(mvec, f == 0 ? mv0.data() : mvB.data(), 4, ST_IN);
			NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {}; ep.pBackbuffer = back; ep.pDepth = dep; ep.pMVecs = mvec; ep.pOutputInterpFrame = out;
			NVSDK_NGX_DLSSG_Opt_Eval_Params op = {};
			op.multiFrameCount = 1; op.multiFrameIndex = 1;
			for (int i = 0; i < 4; i++) { op.cameraViewToClip[i][i] = op.clipToCameraView[i][i] = op.clipToLensClip[i][i] = op.clipToPrevClip[i][i] = op.prevClipToClip[i][i] = 1; }
			op.mvecScale[0] = sign / (float)W; op.mvecScale[1] = sign / (float)H;
			op.cameraNear = 0.1f; op.cameraFar = 1000; op.cameraFOV = 1.5707963f; op.cameraAspectRatio = (float)W / H;
			op.cameraUp[1] = 1; op.cameraRight[0] = 1; op.cameraFwd[2] = 1;
			float px = 1.0f / op.cameraAspectRatio, pz = op.cameraFar / (op.cameraFar - op.cameraNear), pw = -op.cameraNear * pz;
			op.cameraViewToClip[0][0] = px; op.cameraViewToClip[2][2] = pz; op.cameraViewToClip[2][3] = 1; op.cameraViewToClip[3][2] = pw; op.cameraViewToClip[3][3] = 0;
			op.clipToCameraView[0][0] = 1 / px; op.clipToCameraView[2][2] = 0; op.clipToCameraView[2][3] = 1 / pw; op.clipToCameraView[3][2] = 1; op.clipToCameraView[3][3] = -pz / pw;
			op.cameraMotionIncluded = true; op.reset = f == 0;
			op.motionVectorsInvalidValue = 3.402823466e38f;
			op.backbufferSubrectSize = { W, H }; op.outputInterpSubrectSize = { W, H }; op.mvecsSubrectSize = { W, H }; op.depthSubrectSize = { W, H };
			p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)(f + 1));
			QueryPerformanceCounter(&t0);
			r = NGX_D3D12_EVALUATE_DLSSG(list, h, p, &ep, &op);
			run_and_wait();
			QueryPerformanceCounter(&t1);
			if (NVSDK_NGX_FAILED(r)) { printf("Evaluate %d: 0x%08x\n", f, r); return 1; }
			if (f == 1)
			{
				auto img = readback(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				double err; int s = measure_shift(img, &err);
				printf("scale %s1/W: in-between image shifted by %d px (expected %d), residual error %.1f, evaluation %.2f ms\n",
					sign < 0 ? "-" : "+", s, SHIFT / 2, err, (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
				char name[64]; snprintf(name, sizeof(name), "dlssg_out_%s.raw", sign < 0 ? "neg" : "pos");
				FILE *fo = fopen(name, "wb"); if (fo) { fwrite(img.data(), 1, img.size(), fo); fclose(fo); }
			}
		}
		NVSDK_NGX_D3D12_ReleaseFeature(h);
		NVSDK_NGX_D3D12_DestroyParameters(p);
	}
	NVSDK_NGX_D3D12_Shutdown1(dev);
	return 0;
}

// Optional NVIDIA FRUC frame generation engine (needs the NVIDIA Optical Flow SDK FRUC files) + resource extraction.
#include "framegen.h"
#include <stdio.h>
#define TRACE(...) do { fprintf(stderr, "FRUC: " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

// vendor DLLs are resources of the exe (10/11 FRUC, 12 DLSS, 13/14 FSR): written to
// %LOCALAPPDATA%\SaturnProject\nvidia if missing or of a different size
static bool extract(int id, const wchar_t *dir, const wchar_t *name)
{
	HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
	if (!rs) return false;
	void *p = LockResource(LoadResource(nullptr, rs));
	DWORD sz = SizeofResource(nullptr, rs);
	wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\%s", dir, name);
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) && a.nFileSizeLow == sz && !a.nFileSizeHigh) return true;
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wr = 0;
	bool ok = WriteFile(f, p, sz, &wr, nullptr) && wr == sz;
	CloseHandle(f);
	return ok;
}

// also used by dlssg.cpp
bool settings_extract_rc(int id, const wchar_t *dir, const wchar_t *name) { return extract(id, dir, name); }

#ifdef SATURN_NO_FRUC
// built without the optional FRUC engine
bool fruc_load() { return false; }
bool fruc_create(Fruc &, ID3D11Device *, int, int) { return false; }
void fruc_destroy(Fruc &f) { f = Fruc(); }
bool fruc_process(Fruc &, ID3D11DeviceContext4 *, void (*)(), void (*)(), ID3D11Texture2D *, UINT, int, int, double, bool, double,
	ID3D11Texture2D **, bool *, double *) { return false; }
#else
#include "../../third_party/nvofruc/NvOFFRUC.h"

static PtrToFuncNvOFFRUCCreate p_create;
static PtrToFuncNvOFFRUCRegisterResource p_register;
static PtrToFuncNvOFFRUCUnregisterResource p_unregister;
static PtrToFuncNvOFFRUCProcess p_process;
static PtrToFuncNvOFFRUCDestroy p_destroy;

bool fruc_load()
{
	if (p_process) return true;
	wchar_t dir[MAX_PATH];
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return false;
	wcscat(dir, L"\\SaturnProject"); CreateDirectoryW(dir, nullptr);
	wcscat(dir, L"\\nvidia"); CreateDirectoryW(dir, nullptr);
	if (!extract(10, dir, L"NvOFFRUC.dll") || !extract(11, dir, L"cudart64_110.dll")) { TRACE("DLLs not extracted"); return false; }
	wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\NvOFFRUC.dll", dir);
	// LOAD_WITH_ALTERED_SEARCH_PATH: cudart64_110.dll is looked up in the same folder
	HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m) { fprintf(stderr, "FRUC: NvOFFRUC.dll not loaded (%lu)\n", GetLastError()); return false; }
	p_create = (PtrToFuncNvOFFRUCCreate)GetProcAddress(m, CreateProcName);
	p_register = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(m, RegisterResourceProcName);
	p_unregister = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(m, UnregisterResourceProcName);
	p_process = (PtrToFuncNvOFFRUCProcess)GetProcAddress(m, ProcessProcName);
	p_destroy = (PtrToFuncNvOFFRUCDestroy)GetProcAddress(m, DestroyProcName);
	return p_create && p_register && p_unregister && p_process && p_destroy;
}

bool fruc_create(Fruc &f, ID3D11Device *dev, int w, int h)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // ARGBSurface = RGBA. With NV12 FRUC does not write color (green image)
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	for (auto &t : f.in) if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return false;
	for (auto &t : f.out) if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return false;
	ID3D11Device5 *dev5 = nullptr;
	if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5))) return false;
	HRESULT hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&f.fence);
	dev5->Release();
	if (FAILED(hr)) return false;

	NvOFFRUC_CREATE_PARAM cp = {};
	cp.uiWidth = w; cp.uiHeight = h;
	cp.pDevice = dev;
	cp.eResourceType = DirectX11Resource;
	cp.eSurfaceFormat = ARGBSurface;
	cp.eCUDAResourceType = CudaResourceTypeUndefined;
	TRACE("create %dx%d...", w, h);
	NvOFFRUC_STATUS st = p_create(&cp, (NvOFFRUCHandle *)&f.handle);
	TRACE("create -> %d", st);
	if (st != NvOFFRUC_SUCCESS) { fprintf(stderr, "FRUC: create error %d\n", st); return false; }
	NvOFFRUC_REGISTER_RESOURCE_PARAM rp = {};
	for (int i = 0; i < Fruc::NIN; i++) rp.pArrResource[rp.uiCount++] = f.in[i];
	for (int i = 0; i < Fruc::NOUT; i++) rp.pArrResource[rp.uiCount++] = f.out[i];
	rp.pD3D11FenceObj = f.fence;
	st = p_register((NvOFFRUCHandle)f.handle, &rp);
	TRACE("register -> %d", st);
	if (st != NvOFFRUC_SUCCESS) { fprintf(stderr, "FRUC: register error %d\n", st); return false; }
	return true;
}

void fruc_destroy(Fruc &f)
{
	if (f.handle)
	{
		NvOFFRUC_UNREGISTER_RESOURCE_PARAM up = {};
		for (int i = 0; i < Fruc::NIN; i++) up.pArrResource[up.uiCount++] = f.in[i];
		for (int i = 0; i < Fruc::NOUT; i++) up.pArrResource[up.uiCount++] = f.out[i];
		p_unregister((NvOFFRUCHandle)f.handle, &up);
		p_destroy((NvOFFRUCHandle)f.handle);
	}
	for (auto &t : f.in) if (t) { t->Release(); t = nullptr; }
	for (auto &t : f.out) if (t) { t->Release(); t = nullptr; }
	if (f.fence) { f.fence->Release(); f.fence = nullptr; }
	f = Fruc();
}

bool fruc_process(Fruc &f, ID3D11DeviceContext4 *ctx, void (*lock)(), void (*unlock)(),
	ID3D11Texture2D *src, UINT src_slice, int w, int h, double ts, bool skip, double out_ts,
	ID3D11Texture2D **out, bool *repeated, double *ms)
{
	ID3D11Texture2D *in = f.in[f.in_i];
	f.in_i = (f.in_i + 1) % Fruc::NIN;
	f.last_in = in;
	ID3D11Texture2D *dst = f.out[f.out_i];
	f.out_i = (f.out_i + 1) % Fruc::NOUT;

	lock();
	D3D11_BOX box = { 0, 0, 0, (UINT)w, (UINT)h, 1 };
	ctx->CopySubresourceRegion(in, 0, 0, 0, 0, src, src_slice, &box);
	ctx->Signal(f.fence, ++f.fv); // CUDA waits until the copy is done
	ctx->Flush();
	unlock();

	NvOFFRUC_PROCESS_IN_PARAMS ip = {};
	ip.stFrameDataInput.pFrame = in;
	ip.stFrameDataInput.nTimeStamp = ts;
	(void)skip;
	ip.bSkipWarp = 0; // with bSkipWarp FRUC does not signal the output fence: the GPU would stall
	ip.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = f.fv;
	bool rep = false;
	NvOFFRUC_PROCESS_OUT_PARAMS op = {};
	op.stFrameDataOutput.pFrame = dst;
	op.stFrameDataOutput.nTimeStamp = f.have_prev ? out_ts : ts;
	op.stFrameDataOutput.bHasFrameRepetitionOccurred = &rep;
	op.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = ++f.fv;
	LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
	static int calls; bool tr = calls++ < 6;
	if (tr) TRACE("process skip=%d ts=%.3f...", (int)ip.bSkipWarp, ts);
	NvOFFRUC_STATUS st = p_process((NvOFFRUCHandle)f.handle, &ip, &op);
	if (tr) TRACE("process -> %d rep=%d", st, (int)rep);
	QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
	if (ms) *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
	bool had_prev = f.have_prev;
	f.have_prev = true;
	f.prev_ts = ts;
	lock();
	if (st == NvOFFRUC_SUCCESS) ctx->Wait(f.fence, f.fv); // the rest of the GPU waits until CUDA has written the output
	else { ctx->Signal(f.fence, f.fv); f.failures++; }    // CUDA will not signal: we do it, the GPU never stalls
	unlock();
	if (st == NvOFFRUC_SUCCESS) f.failures = 0;
	if (repeated) *repeated = rep;
	if (out) *out = dst;
	return st == NvOFFRUC_SUCCESS && had_prev && !ip.bSkipWarp;
}
#endif // SATURN_NO_FRUC

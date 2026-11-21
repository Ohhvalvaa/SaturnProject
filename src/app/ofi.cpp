#include "ofi.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include "../../third_party/nvof/nvOpticalFlowCommon.h"
#include "../../third_party/nvof/nvOpticalFlowD3D11.h"

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static NV_OF_D3D11_API_FUNCTION_LIST g_of;
static NvOFHandle g_h;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static int g_w, g_h_px, g_fw, g_fh, g_grid = 4;
static ID3D11Texture2D *g_frame[2];          // RGBA copies: P and N alternate
static NvOFGPUBufferHandle g_frame_h[2];
static ID3D11ShaderResourceView *g_frame_srv[2];
static int g_cur = -1;                         // index of N
static ID3D11Texture2D *g_flow[2];           // 0: P->N, 1: N->P (grid, S10.5)
static NvOFGPUBufferHandle g_flow_h[2];
static ID3D11ShaderResourceView *g_flow_srv[2];
static ID3D11Texture2D *g_out[4];
static ID3D11UnorderedAccessView *g_out_uav[4];
static int g_out_i;
static ID3D11ComputeShader *g_cs, *g_cs_alt; // chosen shader and the other one (A/B check only)
static ID3D11Buffer *g_cb;
static ID3D11SamplerState *g_smp;
static bool g_have_pair;

static const char *k_hlsl = R"(
Texture2D<float4> P : register(t0);
Texture2D<float4> N : register(t1);
Texture2D<int2> Ffw : register(t2);   // P -> N
Texture2D<int2> Fbw : register(t3);   // N -> P
RWTexture2D<float4> Out : register(u0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { float t; float grid; float2 pad; float2 inv_size; uint2 size; uint2 fsize; uint2 pad2; };

// flow (in pixels) at point p, bilinearly interpolated between grid cells
float2 flow(Texture2D<int2> F, float2 p)
{
	float2 q = p / grid - 0.5;
	int2 i = (int2)floor(q);
	float2 f = q - i;
	int2 m = (int2)fsize - 1;
	float2 a = F.Load(int3(clamp(i, 0, m), 0));
	float2 b = F.Load(int3(clamp(i + int2(1, 0), 0, m), 0));
	float2 c = F.Load(int3(clamp(i + int2(0, 1), 0, m), 0));
	float2 d = F.Load(int3(clamp(i + int2(1, 1), 0, m), 0));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) / 32.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y) return;
	float2 x = id.xy + 0.5;
	// where pixel x comes from in P and in N: fixed point x = pP + t*Ffw(pP) (two iterations)
	float2 f = flow(Ffw, x); f = flow(Ffw, x - t * f); f = flow(Ffw, x - t * f);
	float2 b = flow(Fbw, x); b = flow(Fbw, x - (1 - t) * b); b = flow(Fbw, x - (1 - t) * b);
	float2 pP = x - t * f;
	float2 pN = x - (1 - t) * b;
	float4 cP = P.SampleLevel(lin, pP * inv_size, 0);
	float4 cN = N.SampleLevel(lin, pN * inv_size, 0);
	// forward/backward consistency: an occluded side (covered or uncovered area) has inconsistent vectors
	float eP = length(flow(Ffw, pP) + flow(Fbw, pP + flow(Ffw, pP)));
	float eN = length(flow(Fbw, pN) + flow(Ffw, pN + flow(Fbw, pN)));
	// sharp weight: a side inconsistent by 2+ px counts almost zero
	float wP = (1 - t) * exp(-eP * eP * 0.5);
	float wN = t * exp(-eN * eN * 0.5);
	float4 c;
	if (wP + wN < 1e-3)
	{
		// both doubtful: the real image closest in time, unwarped (no fragments)
		c = t < 0.5 ? P.SampleLevel(lin, x * inv_size, 0) : N.SampleLevel(lin, x * inv_size, 0);
	}
	else c = (wP * cP + wN * cN) / (wP + wN);
	Out[id.xy] = float4(c.rgb, 1);
}
)";

// Variant 2: per-pixel vector selection. Bilinear flow blends the motion of two nearby objects (a still character
// and a scrolling background): a wrong "in-between" vector appears on the edge = smearing. Here each pixel tries the
// vectors of the neighbouring cells (forward and, negated, backward) and keeps the one for which P and N, sampled
// symmetrically (P at x - t*v, N at x + (1-t)*v), match best on a small 5-point cross. The bilinear vector has an
// advantage (cost x0.85): in flat areas, where all candidates cost the same, nothing changes.
// Where P and N still differ with the best vector (covered/uncovered area) only one side is used, the one with
// consistent forward/backward flow.
static const char *k_hlsl2 = R"(
Texture2D<float4> P : register(t0);
Texture2D<float4> N : register(t1);
Texture2D<int2> Ffw : register(t2);
Texture2D<int2> Fbw : register(t3);
RWTexture2D<float4> Out : register(u0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { float t; float grid; float2 pad; float2 inv_size; uint2 size; uint2 fsize; uint2 pad2; };

float2 flow(Texture2D<int2> F, float2 p)
{
	float2 q = p / grid - 0.5;
	int2 i = (int2)floor(q);
	float2 f = q - i;
	int2 m = (int2)fsize - 1;
	float2 a = F.Load(int3(clamp(i, 0, m), 0));
	float2 b = F.Load(int3(clamp(i + int2(1, 0), 0, m), 0));
	float2 c = F.Load(int3(clamp(i + int2(0, 1), 0, m), 0));
	float2 d = F.Load(int3(clamp(i + int2(1, 1), 0, m), 0));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) / 32.0;
}
float2 cell(Texture2D<int2> F, int2 i) { return F.Load(int3(clamp(i, int2(0, 0), (int2)fsize - 1), 0)) / 32.0; }
float3 SP(float2 p) { return P.SampleLevel(lin, p * inv_size, 0).rgb; }
float3 SN(float2 p) { return N.SampleLevel(lin, p * inv_size, 0).rgb; }

// Catmull-Rom with 5 bilinear taps (corner-less variant): sharp, without visible halos
float3 CR(Texture2D<float4> T, float2 p)
{
	float2 sp = p - 0.5;
	float2 tc = floor(sp) + 0.5;
	float2 f = sp - floor(sp);
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 o12 = w2 / w12;
	float2 t0 = (tc - 1) * inv_size, t3 = (tc + 2) * inv_size, t12 = (tc + o12) * inv_size;
	float3 r = T.SampleLevel(lin, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y)
		+ T.SampleLevel(lin, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y)
		+ T.SampleLevel(lin, t12, 0).rgb * (w12.x * w12.y)
		+ T.SampleLevel(lin, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y)
		+ T.SampleLevel(lin, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
	float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	return saturate(r / wsum);
}

float cost(float2 x, float2 v)
{
	float2 a = x - t * v, b = x + (1 - t) * v;
	float3 d = abs(SP(a) - SN(b)) * 2;
	d += abs(SP(a + float2(2, 0)) - SN(b + float2(2, 0)));
	d += abs(SP(a - float2(2, 0)) - SN(b - float2(2, 0)));
	d += abs(SP(a + float2(0, 2)) - SN(b + float2(0, 2)));
	d += abs(SP(a - float2(0, 2)) - SN(b - float2(0, 2)));
	return d.r + d.g + d.b;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y) return;
	float2 x = id.xy + 0.5;
	float2 v0 = flow(Ffw, x); v0 = flow(Ffw, x - t * v0); v0 = flow(Ffw, x - t * v0);
	float2 best = v0;
	int2 g = (int2)floor(x / grid);
	// the search is only needed where neighbouring vectors differ (edge between two motions): elsewhere bilinear is
	// already right and almost all the work is saved (decode does not wait for the GPU)
	float2 cv[9];
	float spread = 0;
	[unroll] for (int k = 0; k < 9; k++) { cv[k] = cell(Ffw, g + int2(k % 3 - 1, k / 3 - 1)); spread = max(spread, dot(abs(cv[k] - v0), float2(1, 1))); }
	if (spread > 1.0)
	{
		float bc = cost(x, v0) * 0.85;
		[unroll] for (int k = 0; k < 9; k++)
		{
			float c = cost(x, cv[k]);
			if (c < bc) { bc = c; best = cv[k]; }
			float2 v = -cell(Fbw, g + int2(k % 3 - 1, k / 3 - 1));
			c = cost(x, v);
			if (c < bc) { bc = c; best = v; }
		}
	}
	float2 pP = x - t * best, pN = x + (1 - t) * best;
#ifdef SHARP
	// final colors with Catmull-Rom (as sharp as the real images: with bilinear the in-between frames were softer
	// and the sharp/soft alternation at 30 Hz looked like shimmering)
	float3 cP = CR(P, pP), cN = CR(N, pN);
#else
	float3 cP = SP(pP), cN = SN(pN);
#endif
	float d = dot(abs(cP - cN), float3(1, 1, 1));
	float3 c = lerp(cP, cN, t);
#ifdef DEGHOST
	// where warped P and N do not match, averaging overlaps them (double image = ghosting): blend towards
	// the real image closest in time, which stays a single image
	float kd = saturate((d - 0.06) / 0.19);
	c = lerp(c, t < 0.5 ? cP : cN, kd);
#endif
#ifdef STATIC_GUARD
	// static pixel (HUD, text, crosshair, still background): if P and N match at the same spot, the image did not
	// move there and must not be warped with the neighbours' vectors (the background scrolling under the HUD dragged
	// it: shimmering text edges). The threshold is above compression noise.
	float c0 = cost(x, float2(0, 0));
	float ks = saturate((0.30 - c0) / 0.15);
	c = lerp(c, lerp(SP(x), SN(x), t), ks);
	d *= 1 - ks;
#endif
	if (d > 0.25)
	{
		// one side is covered: use the one with consistent forward/backward flow (if equal, the closest in time)
		float eP = length(flow(Ffw, pP) + flow(Fbw, pP + flow(Ffw, pP)));
		float eN = length(flow(Fbw, pN) + flow(Ffw, pN + flow(Fbw, pN)));
		float wP = (1 - t) * exp(-eP * eP * 0.5), wN = t * exp(-eN * eN * 0.5);
		float k = saturate((d - 0.25) * 4);
		float3 one = wP + wN < 1e-3 ? (t < 0.5 ? cP : cN) : (wP * cP + wN * cN) / (wP + wN);
		c = lerp(c, one, k);
	}
	Out[id.xy] = float4(c, 1);
}
)";

// Vectors for DLSS/FSR Frame Generation: N->P (current->previous) flow of the NVOF grid, interpolated per pixel,
// in pixels (R16G16_FLOAT). Where forward and backward disagree (covered/uncovered area) the estimated vector is
// kept: DLSSG has its own network to decide, it only needs guidance.
static const char *k_hlsl_mv = R"(
Texture2D<int2> Fbw : register(t0);
RWTexture2D<float2> Out : register(u0);
cbuffer C : register(b0) { float t; float grid; float2 pad; float2 inv_size; uint2 size; uint2 fsize; uint2 pad2; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= size.x || id.y >= size.y) return;
	float2 q = (id.xy + 0.5) / grid - 0.5;
	int2 i = (int2)floor(q);
	float2 f = q - i;
	int2 m = (int2)fsize - 1;
	float2 a = Fbw.Load(int3(clamp(i, 0, m), 0));
	float2 b = Fbw.Load(int3(clamp(i + int2(1, 0), 0, m), 0));
	float2 c = Fbw.Load(int3(clamp(i + int2(0, 1), 0, m), 0));
	float2 d = Fbw.Load(int3(clamp(i + int2(1, 1), 0, m), 0));
	Out[id.xy] = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) / 32.0;
}
)";
static ID3D11ComputeShader *g_cs_mv;

struct CB { float t, grid, pad[2]; float inv[2]; UINT size[2]; UINT fsize[2]; UINT pad2[2]; };

static int g_perf;
static void release_of(); // NVOF and surfaces; shaders stay (recompiling them is expensive)

bool ofi_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h, int perf)
{
	char *pe = getenv("SATURN_OF_PERF"); if (pe) perf = atoi(pe); // 5 slow (best quality), 10 medium, 20 fast
	if (g_h && w == g_w && h == g_h_px && perf == g_perf) return true;
	release_of();
	g_perf = perf;
	g_dev = dev; g_ctx = ctx; g_w = w; g_h_px = h; g_fw = (w + 3) / 4; g_fh = (h + 3) / 4;
	HMODULE m = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m) { fprintf(stderr, "NVOF: nvofapi64.dll missing\n"); return false; }
	auto create = (NV_OF_STATUS(NVOFAPI *)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST *))GetProcAddress(m, "NvOFAPICreateInstanceD3D11");
	if (!create || create(NV_OF_API_VERSION, &g_of) != NV_OF_SUCCESS) { fprintf(stderr, "NVOF: instance not created\n"); return false; }
	if (g_of.nvCreateOpticalFlowD3D11(dev, ctx, &g_h) != NV_OF_SUCCESS) { fprintf(stderr, "NVOF: create failed\n"); return false; }
	// finest supported grid (1x1 on Ada) and best quality: clean edges, no block steps
	uint32_t grids[8] = {}, gsz = sizeof(grids);
	g_grid = 4;
	if (g_of.nvOFGetCaps(g_h, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids, &gsz) == NV_OF_SUCCESS)
		for (uint32_t i = 0; i < gsz && i < 8; i++) if (grids[i] && (int)grids[i] < g_grid) g_grid = (int)grids[i];
	if (g_grid < 2) g_grid = 2; // 1x1 at best quality saturates the GPU (90 fps instead of 120, measured); 2x2 keeps up
	char *ge = getenv("SATURN_OF_GRID"); if (ge && (atoi(ge) == 1 || atoi(ge) == 2 || atoi(ge) == 4)) g_grid = atoi(ge);
	g_fw = (w + g_grid - 1) / g_grid; g_fh = (h + g_grid - 1) / g_grid;
	NV_OF_INIT_PARAMS ip = {};
	ip.width = w; ip.height = h;
	ip.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)g_grid;
	ip.mode = NV_OF_MODE_OPTICALFLOW;
	ip.perfLevel = (NV_OF_PERF_LEVEL)perf;
	ip.predDirection = NV_OF_PRED_DIRECTION_BOTH;
	ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
	NV_OF_STATUS st = g_of.nvOFInit(g_h, &ip);
	fprintf(stderr, "NVOF: grid %dx%d, level %d (%s)\n", g_grid, g_grid, perf, perf <= 5 ? "best quality" : perf <= 10 ? "medium" : "fast");
	if (st != NV_OF_SUCCESS)
	{
		char err[256] = { 0 }; uint32_t n = sizeof(err); g_of.nvOFGetLastError(g_h, err, &n);
		fprintf(stderr, "NVOF: init error %d %s\n", st, err);
		return false;
	}
	// accepted formats: ask for the whole list (a single DXGI_FORMAT is not enough) and pick 8-bit BGRA (no RGBA)
	DXGI_FORMAT fin = DXGI_FORMAT_B8G8R8A8_UNORM, fout = DXGI_FORMAT_R16G16_SINT;
	for (int u = 0; u < 2; u++)
	{
		NV_OF_BUFFER_USAGE usage = u ? NV_OF_BUFFER_USAGE_OUTPUT : NV_OF_BUFFER_USAGE_INPUT;
		uint32_t cnt = 0;
		g_of.nvOFGetSurfaceFormatCountD3D11(g_h, usage, NV_OF_MODE_OPTICALFLOW, &cnt);
		DXGI_FORMAT list[32] = {};
		if (cnt > 32) cnt = 32;
		if (cnt) g_of.nvOFGetSurfaceFormatD3D11(g_h, usage, NV_OF_MODE_OPTICALFLOW, list);
		fprintf(stderr, "NVOF: %s formats:", u ? "flow" : "input");
		bool has_rgba = false;
		for (uint32_t i = 0; i < cnt; i++) { fprintf(stderr, " %d", list[i]); if (list[i] == DXGI_FORMAT_B8G8R8A8_UNORM) has_rgba = true; }
		fprintf(stderr, "\n");
		if (u == 0 && cnt && !has_rgba) fin = list[0];
		if (u == 1 && cnt) fout = list[0];
	}
	fprintf(stderr, "NVOF: %dx%d, input %d, flow %d\n", w, h, fin, fout);
	if (fin != DXGI_FORMAT_B8G8R8A8_UNORM) { fprintf(stderr, "NVOF: BGRA not accepted\n"); return false; }

	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fin; d.SampleDesc.Count = 1;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS; // like the NVIDIA sample
	for (int i = 0; i < 2; i++)
	{
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_frame[i]))) return false;
		if (g_of.nvOFRegisterResourceD3D11(g_h, g_frame[i], &g_frame_h[i]) != NV_OF_SUCCESS) { fprintf(stderr, "NVOF: register input\n"); return false; }
		dev->CreateShaderResourceView(g_frame[i], nullptr, &g_frame_srv[i]);
	}
	D3D11_TEXTURE2D_DESC fd = d;
	fd.Width = g_fw; fd.Height = g_fh; fd.Format = fout; fd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	for (int i = 0; i < 2; i++)
	{
		if (FAILED(dev->CreateTexture2D(&fd, nullptr, &g_flow[i]))) return false;
		if (g_of.nvOFRegisterResourceD3D11(g_h, g_flow[i], &g_flow_h[i]) != NV_OF_SUCCESS) { fprintf(stderr, "NVOF: register flow\n"); return false; }
		dev->CreateShaderResourceView(g_flow[i], nullptr, &g_flow_srv[i]);
	}
	D3D11_TEXTURE2D_DESC od = d;
	// generated images in the same format as the real ones (BGRA): with RGBA the scaler/RTX VSR made them ~15% sharper
	// than the real ones (measured on screen) and the alternation pulsed. RGBA only if BGRA cannot be written by a shader.
	UINT fs2 = 0;
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2 = { DXGI_FORMAT_B8G8R8A8_UNORM, 0 };
	if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2)))) fs2 = f2.OutFormatSupport2;
	od.Format = (fs2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	fprintf(stderr, "NVOF: generated images in %s\n", od.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA" : "RGBA");
	od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	for (int i = 0; i < 4; i++)
	{
		if (FAILED(dev->CreateTexture2D(&od, nullptr, &g_out[i]))) return false;
		dev->CreateUnorderedAccessView(g_out[i], nullptr, &g_out_uav[i]);
	}
	// SATURN_OF_SHADER: 1 = bilinear flow (first), 2 = per-pixel vector choice, 3 (default) = 2 + sharp colors
	// (Catmull-Rom) + no double images where P and N do not match. The second compiled shader is only for the
	// A/B check (SATURN_OF_EVAL): 2 if the main one is 3, otherwise 3.
	char *sv = getenv("SATURN_OF_SHADER");
	int main_v = sv && atoi(sv) >= 1 && atoi(sv) <= 3 ? atoi(sv) : 3;
	bool first = !g_cs;
	if (first) fprintf(stderr, "NVOF: shader %d\n", main_v);
	for (int k = 0; k < 2 && first; k++)
	{
		int v = k == 0 ? main_v : (main_v == 3 ? 2 : 3);
		const char *src = v == 1 ? k_hlsl : k_hlsl2;
		D3D_SHADER_MACRO m3[] = { { "SHARP", "1" }, { "DEGHOST", "1" }, { "STATIC_GUARD", "1" }, { nullptr, nullptr } };
		ID3DBlob *code = nullptr, *errs = nullptr;
		if (FAILED(D3DCompile(src, strlen(src), "ofi", v == 3 ? m3 : nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)))
		{
			fprintf(stderr, "NVOF: shader not compiled: %s\n", errs ? (const char *)errs->GetBufferPointer() : "?");
			release(errs);
			if (k == 0) return false;
			continue;
		}
		dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, k == 0 ? &g_cs : &g_cs_alt);
		release(code);
	}
	if (!g_cs_mv)
	{
		ID3DBlob *code = nullptr, *errs = nullptr;
		if (SUCCEEDED(D3DCompile(k_hlsl_mv, strlen(k_hlsl_mv), "ofi_mv", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)))
		{ dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs_mv); code->Release(); }
		else fprintf(stderr, "NVOF: vector shader not compiled: %s\n", errs ? (const char *)errs->GetBufferPointer() : "?");
		release(errs);
	}
	D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (!g_cb) dev->CreateBuffer(&bd, nullptr, &g_cb);
	D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	if (!g_smp) dev->CreateSamplerState(&sd, &g_smp);
	g_cur = -1; g_have_pair = false;
	return g_cs && g_cb && g_smp;
}

void ofi_fini()
{
	release_of();
	release(g_cs); release(g_cs_alt); release(g_cs_mv); release(g_cb); release(g_smp);
}

static void release_of()
{
	if (g_h)
	{
		for (int i = 0; i < 2; i++) { if (g_frame_h[i]) g_of.nvOFUnregisterResourceD3D11(g_frame_h[i]); if (g_flow_h[i]) g_of.nvOFUnregisterResourceD3D11(g_flow_h[i]); g_frame_h[i] = g_flow_h[i] = nullptr; }
		g_of.nvOFDestroy(g_h);
		g_h = nullptr;
	}
	for (int i = 0; i < 2; i++) { release(g_frame_srv[i]); release(g_frame[i]); release(g_flow_srv[i]); release(g_flow[i]); }
	for (int i = 0; i < 4; i++) { release(g_out_uav[i]); release(g_out[i]); }
	g_w = g_h_px = 0;
	g_cur = -1; g_have_pair = false;
}

bool ofi_push(ID3D11Texture2D *src, bool flow)
{
	if (!g_h) return false;
	g_cur = (g_cur + 1) & 1;
	D3D11_BOX box = { 0, 0, 0, (UINT)g_w, (UINT)g_h_px, 1 };
	g_ctx->CopySubresourceRegion(g_frame[g_cur], 0, 0, 0, 0, src, 0, &box);
	if (!g_have_pair && g_cur == 1) g_have_pair = true;
	if (!g_have_pair || !flow) return false;
	int prev = g_cur ^ 1;
	NV_OF_EXECUTE_INPUT_PARAMS in = {};
	in.inputFrame = g_frame_h[prev];      // P
	in.referenceFrame = g_frame_h[g_cur]; // N
	in.disableTemporalHints = NV_OF_FALSE;
	NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
	out.outputBuffer = g_flow_h[0];       // P -> N
	out.bwdOutputBuffer = g_flow_h[1];    // N -> P
	NV_OF_STATUS st = g_of.nvOFExecute(g_h, &in, &out);
	if (st != NV_OF_SUCCESS)
	{
		static int told;
		if (told++ < 3) { char err[256] = { 0 }; uint32_t n = sizeof(err); g_of.nvOFGetLastError(g_h, err, &n); fprintf(stderr, "NVOF: execute error %d %s\n", st, err); }
		return false;
	}
	return true;
}

ID3D11Texture2D *ofi_interp(float t) { return ofi_interp_alt(t, false); }

ID3D11Texture2D *ofi_interp_alt(float t, bool alt)
{
	if (alt && !g_cs_alt) return nullptr;
	int i = g_out_i; g_out_i = (g_out_i + 1) & 3;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
	CB c = {}; c.t = t; c.grid = (float)g_grid; c.inv[0] = 1.0f / g_w; c.inv[1] = 1.0f / g_h_px; c.size[0] = g_w; c.size[1] = g_h_px; c.fsize[0] = g_fw; c.fsize[1] = g_fh;
	memcpy(m.pData, &c, sizeof(c));
	g_ctx->Unmap(g_cb, 0);
	ID3D11ShaderResourceView *srv[4] = { g_frame_srv[g_cur ^ 1], g_frame_srv[g_cur], g_flow_srv[0], g_flow_srv[1] };
	g_ctx->CSSetShader(alt ? g_cs_alt : g_cs, nullptr, 0);
	g_ctx->CSSetShaderResources(0, 4, srv);
	g_ctx->CSSetUnorderedAccessViews(0, 1, &g_out_uav[i], nullptr);
	g_ctx->CSSetConstantBuffers(0, 1, &g_cb);
	g_ctx->CSSetSamplers(0, 1, &g_smp);
	g_ctx->Dispatch((g_w + 7) / 8, (g_h_px + 7) / 8, 1);
	ID3D11ShaderResourceView *nsrv[4] = {};
	ID3D11UnorderedAccessView *nuav = nullptr;
	g_ctx->CSSetShaderResources(0, 4, nsrv);
	g_ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
	return g_out[i];
}

ID3D11Texture2D *ofi_current() { return g_cur >= 0 ? g_frame[g_cur] : nullptr; }
ID3D11Texture2D *ofi_previous() { return g_cur >= 0 ? g_frame[g_cur ^ 1] : nullptr; }

void ofi_reset() { g_cur = -1; g_have_pair = false; } // new session: no pair with the old image

bool ofi_motion(ID3D11UnorderedAccessView *out)
{
	if (!g_h || !g_have_pair || !g_cs_mv || !out) return false;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
	CB c = {}; c.grid = (float)g_grid; c.inv[0] = 1.0f / g_w; c.inv[1] = 1.0f / g_h_px; c.size[0] = g_w; c.size[1] = g_h_px; c.fsize[0] = g_fw; c.fsize[1] = g_fh;
	memcpy(m.pData, &c, sizeof(c));
	g_ctx->Unmap(g_cb, 0);
	g_ctx->CSSetShader(g_cs_mv, nullptr, 0);
	g_ctx->CSSetShaderResources(0, 1, &g_flow_srv[1]);
	g_ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
	g_ctx->CSSetConstantBuffers(0, 1, &g_cb);
	g_ctx->Dispatch((g_w + 7) / 8, (g_h_px + 7) / 8, 1);
	ID3D11ShaderResourceView *nsrv = nullptr; ID3D11UnorderedAccessView *nuav = nullptr;
	g_ctx->CSSetShaderResources(0, 1, &nsrv);
	g_ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
	return true;
}

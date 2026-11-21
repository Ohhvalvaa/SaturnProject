// Test bench for src/app/dlssg.cpp without a PS5: D3D11 device like the app, dlssg_init + 3 dlssg_generate.
// Usage: dlssg_bench.exe [fsr]   (the vendor DLLs must already be in %LOCALAPPDATA%\SaturnProject\nvidia)
#include <windows.h>
#include <d3d11_4.h>
#include <stdio.h>
#include <string.h>
#include "../src/app/dlssg.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
bool settings_extract_rc(int, const wchar_t *, const wchar_t *) { return true; } // DLLs already extracted by the app
int main(int argc, char **argv)
{
	ID3D11Device *dev; ID3D11DeviceContext *ctx;
	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 1;
	ID3D11DeviceContext4 *ctx4; ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4);
	bool fsr = argc > 1 && !strcmp(argv[1], "fsr");
	bool ok = dlssg_init(dev, 1920, 1080, fsr ? FG12_FSR : FG12_DLSS);
	printf("init (%s): %d\n", fsr ? "FSR 3" : "DLSS", ok);
	if (!ok) return 1;
	for (int i = 0; i < 3; i++)
	{
		double ms = 0;
		ID3D11Texture2D *out = dlssg_generate(ctx4, i == 0, 16.7, &ms);
		printf("generate %d: %s %.2f ms\n", i, out ? "ok" : "FAILED", ms);
	}
	dlssg_fini();
	printf("done\n");
	return 0;
}

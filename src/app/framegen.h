// NB: FRUC supports ONE instance per process (a second one puts the first in error 16).
// Frame generation with NVIDIA FRUC (Optical Flow SDK): RTX hardware optical flow + CUDA (optional fallback).
// RGBA surfaces shared D3D11<->CUDA, synchronized with ID3D11Fence.
#pragma once
#include <d3d11_4.h>

struct Fruc
{
	void *handle = nullptr;
	ID3D11Fence *fence = nullptr;
	UINT64 fv = 0;
	static const int NIN = 4, NOUT = 4;
	ID3D11Texture2D *in[NIN] = {}, *out[NOUT] = {};
	int in_i = 0, out_i = 0;
	ID3D11Texture2D *last_in = nullptr; // copy of the last frame passed in (used as "P")
	bool have_prev = false;
	double prev_ts = 0;
	int failures = 0; // consecutive errors: above a threshold generation is switched off
};

// loads NvOFFRUC.dll; false if the GPU/driver does not support it
bool fruc_load();
bool fruc_create(Fruc &f, ID3D11Device *dev, int w, int h);
void fruc_destroy(Fruc &f);
// Passes the frame (src, slice) with its time in seconds to FRUC. If skip is false it returns
// in *out the image at time out_ts between the previous frame and this one.
// lock/unlock protect the D3D11 context shared with the decoder.
bool fruc_process(Fruc &f, ID3D11DeviceContext4 *ctx, void (*lock)(), void (*unlock)(),
	ID3D11Texture2D *src, UINT src_slice, int w, int h, double ts, bool skip, double out_ts,
	ID3D11Texture2D **out, bool *repeated, double *ms);

// Interpolation with NVIDIA Optical Flow (NVOF, RTX hardware) + our own shader.
// Unlike FRUC it has no hidden state: for each pair P,N it computes the flow in both directions and from
// there any time t (1/4, 1/2, 3/4) in ~0.3 ms each. Surfaces at the stream resolution.
#pragma once
#include <d3d11.h>

// perf: NVOF level (5 slow/best, 10 medium, 20 fast); if it changes, the optical flow object is recreated
bool ofi_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h, int perf);
void ofi_fini();
// copies src as the new frame N (the previous one becomes P) and computes the P<->N flow.
// Call with the D3D11 context locked (hw_lock). false = flow not available.
// flow = false: copy only (the new image duplicates the previous one: no flow to compute)
bool ofi_push(ID3D11Texture2D *src, bool flow = true);
// generates the image at time t (0 = P, 1 = N) into one of the output surfaces (ring of 4)
ID3D11Texture2D *ofi_interp(float t);
ID3D11Texture2D *ofi_interp_alt(float t, bool alt); // alt = the other shader (A/B check)
ID3D11Texture2D *ofi_current();  // copy of N
ID3D11Texture2D *ofi_previous(); // copy of P
// per-pixel current->previous motion vectors (for DLSSG/FSR), from the last pair's flow
bool ofi_motion(ID3D11UnorderedAccessView *out);
void ofi_reset();                // forget P (new session)

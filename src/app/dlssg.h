// Frame generation with NVIDIA DLSS Frame Generation (NGX DLSSG) or AMD FSR 3 Frame Generation.
// Both are D3D12-only: the app stays on D3D11 (decode, optical flow, scaling) and hands a D3D12 device on the
// same GPU three shared surfaces (image, motion vectors, generated image), ordered by a shared fence:
// everything stays on the GPU, the CPU never waits.
// Inputs (checked with tools/dlssg_probe.cpp on an RTX 4080: 1.5 ms at 1080p): current real image BGRA,
// current->previous motion vectors in pixels (R16G16_FLOAT, scale -1/width),
// constant depth 0.9, simulated static camera.
#pragma once
#include <d3d11_4.h>

// Two engines on the same D3D12 bridge: DLSS Frame Generation (NVIDIA RTX 40 and newer) and AMD FSR 3 Frame Generation
// (any DirectX 12 GPU: NVIDIA, AMD, Intel).
enum Fg12Backend { FG12_DLSS, FG12_FSR };
// creates the D3D12 device and the chosen engine for w x h images. false = not available (reason goes to the log)
bool dlssg_init(ID3D11Device *dev11, int w, int h, int backend);
void dlssg_fini();
int dlssg_width();
int dlssg_height();
// D3D11 surfaces to fill before dlssg_generate: the current real image and the vectors (UAV for the shader)
ID3D11Texture2D *dlssg_input();
ID3D11UnorderedAccessView *dlssg_motion_uav();
// generates the image halfway between the previous real image and the current one (already copied to dlssg_input).
// Call with the D3D11 context locked; reset = no valid previous image (first frame, scene change).
// Returns the D3D11 surface holding the generated image (ready for the D3D11 context) or nullptr.
// elapsed_ms = time between the two real images (used by FSR)
ID3D11Texture2D *dlssg_generate(ID3D11DeviceContext4 *ctx, bool reset, double elapsed_ms, double *gpu_ms);
const char *dlssg_name(); // "DLSS" or "FSR 3"
ID3D11Texture2D *dlssg_current(); // the real image just passed to dlssg_generate (shown after the generated one)
// DLSS only, 30 fps source: DLSS twice in cascade. The first generates the midpoint between the previous and the current
// real image; the second receives the sequence (previous, midpoint, current) and generates 1/4 and 3/4: 30 -> 120.
bool dlssg_generate4(ID3D11DeviceContext4 *ctx, bool reset, ID3D11Texture2D **q1, ID3D11Texture2D **mid, ID3D11Texture2D **q3, double *gpu_ms);

// DXVK Native D3D9 smoke test: clear, vertex-coloured triangle, textured quad.
// Draws into a 256x256 back buffer of an SDL2 window (DXVK_WSI_DRIVER=SDL2), reads the back buffer
// back with GetRenderTargetData + LockRect, and checks pixel values.
// Shaders are hand-assembled vs_2_0 / ps_2_0 token streams (no D3DX, no HLSL).

#include <windows.h>
#include <d3d9.h>
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <unistd.h>
#include <vector>

static const UINT W = 256, H = 256;
static const D3DCOLOR CLEAR = D3DCOLOR_XRGB(32, 64, 96);
static int g_fail = 0;

// Token helpers. Register type: bits 28-30 = low 3 bits, bits 11-12 = high 2 bits.
static DWORD regbits(DWORD type, DWORD num) {
  return 0x80000000u | ((type & 7u) << 28) | ((type >> 3) << 11) | num;
}
static DWORD dst(DWORD type, DWORD num) { return regbits(type, num) | 0x000F0000u; } // .xyzw
static DWORD src(DWORD type, DWORD num) { return regbits(type, num) | 0x00E40000u; } // .xyzw
enum { R_TEMP = 0, R_INPUT = 1, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5,
       R_TEXCRDOUT = 6, R_COLOROUT = 8, R_SAMPLER = 10 };
static const DWORD OP_MOV = 0x02000001u, OP_DCL = 0x0200001Fu, OP_TEX = 0x03000042u, OP_END = 0x0000FFFFu;

// vs_2_0: dcl_position v0; dcl_color v1; mov oPos, v0; mov oD0, v1
static const DWORD vs_color[] = {
  0xFFFE0200u,
  OP_DCL, 0x80000000u, dst(R_INPUT, 0),
  OP_DCL, 0x8000000Au, dst(R_INPUT, 1),
  OP_MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0),
  OP_MOV, dst(R_ATTROUT, 0), src(R_INPUT, 1),
  OP_END };
// ps_2_0: dcl v0; mov oC0, v0
static const DWORD ps_color[] = {
  0xFFFF0200u,
  OP_DCL, 0x80000000u, dst(R_INPUT, 0),
  OP_MOV, dst(R_COLOROUT, 0), src(R_INPUT, 0),
  OP_END };
// vs_2_0: dcl_position v0; dcl_texcoord v1; mov oPos, v0; mov oT0, v1
static const DWORD vs_tex[] = {
  0xFFFE0200u,
  OP_DCL, 0x80000000u, dst(R_INPUT, 0),
  OP_DCL, 0x80000005u, dst(R_INPUT, 1),
  OP_MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0),
  OP_MOV, dst(R_TEXCRDOUT, 0), src(R_INPUT, 1),
  OP_END };
// ps_2_0: dcl t0; dcl_2d s0; texld r0, t0, s0; mov oC0, r0
static const DWORD ps_tex[] = {
  0xFFFF0200u,
  OP_DCL, 0x80000000u, dst(R_TEXTURE, 0),
  OP_DCL, 0x90000000u, dst(R_SAMPLER, 0),
  OP_TEX, dst(R_TEMP, 0), src(R_TEXTURE, 0), src(R_SAMPLER, 0),
  OP_MOV, dst(R_COLOROUT, 0), src(R_TEMP, 0),
  OP_END };

struct ColVert { float x, y, z, w; D3DCOLOR c; };
struct TexVert { float x, y, z, w, u, v; };

static const ColVert tri[3] = {
  { -0.8f, -0.8f, 0.5f, 1.f, D3DCOLOR_XRGB(255, 0, 0) },
  {  0.8f, -0.8f, 0.5f, 1.f, D3DCOLOR_XRGB(0, 255, 0) },
  {  0.0f,  0.8f, 0.5f, 1.f, D3DCOLOR_XRGB(0, 0, 255) } };

static D3DCOLOR texel(int i, int j) { return D3DCOLOR_XRGB(40 + 60 * i, 40 + 60 * j, 200); }

#define CHECK_HR(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
  std::printf("FAIL call %s -> hr=0x%08x\n", #expr, (unsigned)hr_); return 2; } } while (0)

static void report(bool ok, const char* name, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  std::printf("%s %s: ", ok ? "PASS" : "FAIL", name);
  std::vprintf(fmt, ap); std::printf("\n");
  va_end(ap);
  if (!ok) g_fail++;
}

static bool near(D3DCOLOR a, int r, int g, int b, int tol) {
  return std::abs(int((a >> 16) & 255) - r) <= tol && std::abs(int((a >> 8) & 255) - g) <= tol &&
         std::abs(int(a & 255) - b) <= tol;
}

// Copy the back buffer to system memory and return its pixels (0x00RRGGBB per pixel).
static HRESULT readback(IDirect3DDevice9* dev, IDirect3DSurface9* sys, std::vector<D3DCOLOR>& px) {
  IDirect3DSurface9* bb = nullptr;
  HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  if (FAILED(hr)) return hr;
  hr = dev->GetRenderTargetData(bb, sys);
  bb->Release();
  if (FAILED(hr)) return hr;
  D3DLOCKED_RECT lr;
  hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
  if (FAILED(hr)) return hr;
  px.resize(W * H);
  for (UINT y = 0; y < H; y++)
    for (UINT x = 0; x < W; x++)
      px[y * W + x] = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x] & 0x00FFFFFFu;
  return sys->UnlockRect();
}

// Expected interpolated triangle colour at pixel (x, y); D3D9 pixel centres are integer screen coords.
static bool tri_expect(int x, int y, int rgb[3]) {
  float sx[3], sy[3];
  for (int i = 0; i < 3; i++) { sx[i] = (tri[i].x + 1.f) * 0.5f * W; sy[i] = (1.f - tri[i].y) * 0.5f * H; }
  float px = float(x), py = float(y);
  float d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
  float l0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / d;
  float l1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / d;
  float l2 = 1.f - l0 - l1;
  rgb[0] = int(std::lround(l0 * 255)); rgb[1] = int(std::lround(l1 * 255)); rgb[2] = int(std::lround(l2 * 255));
  return l0 > 0.02f && l1 > 0.02f && l2 > 0.02f;
}

static void draw_triangle(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl, IDirect3DVertexBuffer9* vb,
                          IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps) {
  dev->SetVertexDeclaration(decl);
  dev->SetVertexShader(vs);
  dev->SetPixelShader(ps);
  dev->SetStreamSource(0, vb, 0, sizeof(ColVert));
  dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
}

// DXVK reports fatal errors as C++ exceptions. Exit cleanly instead of abort(), so a failed
// run prints the error and leaves no crash report or "quit unexpectedly" dialog.
static void on_terminate() {
  const char* what = "unknown exception";
  try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
  catch (const std::exception& e) { what = e.what(); }
  catch (...) {}
  std::fprintf(stderr, "FAIL uncaught exception: %s\n", what);
  std::fflush(stdout);
  _exit(2);
}

int main() {
  std::set_terminate(on_terminate);
  setenv("DXVK_WSI_DRIVER", "SDL2", 0);
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::printf("FAIL SDL_Init: %s\n", SDL_GetError()); return 2; }
  SDL_Window* win = SDL_CreateWindow("dxvk d3d9 test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_VULKAN);
  if (!win) { std::printf("FAIL SDL_CreateWindow: %s\n", SDL_GetError()); return 2; }

  IDirect3D9* d3d = nullptr;
  try { d3d = Direct3DCreate9(D3D_SDK_VERSION); }
  catch (...) { std::printf("FAIL Direct3DCreate9 threw (see DXVK log for the DxvkError text)\n"); return 2; }
  if (!d3d) { std::printf("FAIL Direct3DCreate9 returned null\n"); return 2; }
  D3DADAPTER_IDENTIFIER9 id = {};
  d3d->GetAdapterIdentifier(0, 0, &id);
  std::printf("adapter: %s / %s\n", id.Description, id.Driver);

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = reinterpret_cast<HWND>(win);
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* dev = nullptr;
  try { CHECK_HR(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)); }
  catch (...) { std::printf("FAIL CreateDevice threw (see DXVK log for the DxvkError text)\n"); return 2; }
  D3DCAPS9 caps = {};
  dev->GetDeviceCaps(&caps);
  std::printf("caps: VS %u.%u PS %u.%u\n", (unsigned)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
              (unsigned)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
              (unsigned)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion));

  IDirect3DSurface9* sys = nullptr;
  CHECK_HR(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
  IDirect3DVertexShader9 *vsC = nullptr, *vsT = nullptr;
  IDirect3DPixelShader9 *psC = nullptr, *psT = nullptr;
  CHECK_HR(dev->CreateVertexShader(vs_color, &vsC));
  CHECK_HR(dev->CreatePixelShader(ps_color, &psC));
  CHECK_HR(dev->CreateVertexShader(vs_tex, &vsT));
  CHECK_HR(dev->CreatePixelShader(ps_tex, &psT));

  const D3DVERTEXELEMENT9 elC[] = {
    { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
  const D3DVERTEXELEMENT9 elT[] = {
    { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
  IDirect3DVertexDeclaration9 *declC = nullptr, *declT = nullptr;
  CHECK_HR(dev->CreateVertexDeclaration(elC, &declC));
  CHECK_HR(dev->CreateVertexDeclaration(elT, &declT));

  IDirect3DVertexBuffer9* vb = nullptr;
  CHECK_HR(dev->CreateVertexBuffer(sizeof(tri), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr));
  void* vbp = nullptr;
  CHECK_HR(vb->Lock(0, sizeof(tri), &vbp, 0));
  std::memcpy(vbp, tri, sizeof(tri));
  vb->Unlock();

  IDirect3DTexture9* tex = nullptr;
  CHECK_HR(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr));
  D3DLOCKED_RECT tl;
  CHECK_HR(tex->LockRect(0, &tl, nullptr, 0));
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++)
      reinterpret_cast<DWORD*>(static_cast<BYTE*>(tl.pBits) + j * tl.Pitch)[i] = texel(i, j) | 0xFF000000u;
  tex->UnlockRect(0);

  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

  std::vector<D3DCOLOR> px;

  // 1. Clear.
  CHECK_HR(dev->Clear(0, nullptr, D3DCLEAR_TARGET, CLEAR, 1.f, 0));
  CHECK_HR(readback(dev, sys, px));
  {
    size_t bad = 0; D3DCOLOR firstBad = 0;
    for (D3DCOLOR c : px) if (c != (CLEAR & 0xFFFFFFu)) { if (!bad) firstBad = c; bad++; }
    report(bad == 0, "clear", "expected 0x%06x at all %u pixels; (0,0)=0x%06x (128,128)=0x%06x (255,255)=0x%06x; mismatches=%zu first=0x%06x",
           (unsigned)(CLEAR & 0xFFFFFFu), W * H, (unsigned)px[0], (unsigned)px[128 * W + 128], (unsigned)px[W * H - 1], bad, (unsigned)firstBad);
  }
  dev->Present(nullptr, nullptr, nullptr, nullptr);

  // 2. Vertex-coloured triangle.
  CHECK_HR(dev->Clear(0, nullptr, D3DCLEAR_TARGET, CLEAR, 1.f, 0));
  CHECK_HR(dev->BeginScene());
  draw_triangle(dev, declC, vb, vsC, psC);
  CHECK_HR(dev->EndScene());
  CHECK_HR(readback(dev, sys, px));
  {
    const int pts[][2] = { { 128, 162 }, { 40, 222 }, { 216, 222 }, { 128, 40 }, { 100, 180 }, { 160, 120 } };
    for (auto& p : pts) {
      int e[3]; tri_expect(p[0], p[1], e);
      D3DCOLOR c = px[p[1] * W + p[0]];
      char name[48]; std::snprintf(name, sizeof(name), "triangle(%d,%d)", p[0], p[1]);
      report(near(c, e[0], e[1], e[2], 4), name, "got 0x%06x expected ~%02x%02x%02x (tol 4)", (unsigned)c, e[0], e[1], e[2]);
    }
    const int out[][2] = { { 10, 10 }, { 245, 10 }, { 128, 250 } };
    for (auto& p : out) {
      D3DCOLOR c = px[p[1] * W + p[0]];
      char name[48]; std::snprintf(name, sizeof(name), "triangle-outside(%d,%d)", p[0], p[1]);
      report(c == (CLEAR & 0xFFFFFFu), name, "got 0x%06x expected 0x%06x", (unsigned)c, (unsigned)(CLEAR & 0xFFFFFFu));
    }
  }
  dev->Present(nullptr, nullptr, nullptr, nullptr);

  // 3. Textured quad covering the whole target; each 4x4 texel covers a 64x64 block.
  const TexVert quad[4] = { { -1, 1, 0.5f, 1, 0, 0 }, { 1, 1, 0.5f, 1, 1, 0 }, { -1, -1, 0.5f, 1, 0, 1 }, { 1, -1, 0.5f, 1, 1, 1 } };
  CHECK_HR(dev->Clear(0, nullptr, D3DCLEAR_TARGET, CLEAR, 1.f, 0));
  CHECK_HR(dev->BeginScene());
  dev->SetVertexDeclaration(declT);
  dev->SetVertexShader(vsT);
  dev->SetPixelShader(psT);
  dev->SetTexture(0, tex);
  CHECK_HR(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexVert)));
  CHECK_HR(dev->EndScene());
  CHECK_HR(readback(dev, sys, px));
  {
    int ok = 0; char buf[1024]; int n = 0;
    for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++) {
        D3DCOLOR c = px[(32 + 64 * j) * W + (32 + 64 * i)], e = texel(i, j) & 0xFFFFFFu;
        if (c == e) ok++;
        n += std::snprintf(buf + n, sizeof(buf) - n, "%s(%d,%d)=0x%06x/0x%06x", n ? " " : "", i, j, (unsigned)c, (unsigned)e);
      }
    report(ok == 16, "textured-quad", "%d/16 texel centres exact; got/expected %s", ok, buf);
  }
  dev->Present(nullptr, nullptr, nullptr, nullptr);
  dev->SetTexture(0, nullptr);

  // 4. Timing: 300 presents of the triangle scene.
  std::vector<double> ms;
  auto t0 = std::chrono::steady_clock::now(), prev = t0;
  for (int f = 0; f < 300; f++) {
    SDL_Event ev; while (SDL_PollEvent(&ev)) {}
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, CLEAR, 1.f, 0);
    dev->BeginScene();
    draw_triangle(dev, declC, vb, vsC, psC);
    dev->EndScene();
    HRESULT hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) { report(false, "present-loop", "Present frame %d hr=0x%08x", f, (unsigned)hr); break; }
    auto now = std::chrono::steady_clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
    prev = now;
  }
  if (!ms.empty()) {
    double total = std::chrono::duration<double, std::milli>(prev - t0).count();
    std::vector<double> s = ms; std::sort(s.begin(), s.end());
    std::printf("timing: %zu presents in %.1f ms; mean %.3f ms; median %.3f ms; min %.3f ms; max %.3f ms (interval IMMEDIATE)\n",
                ms.size(), total, total / ms.size(), s[s.size() / 2], s.front(), s.back());
  }

  tex->Release(); vb->Release(); declC->Release(); declT->Release();
  vsC->Release(); psC->Release(); vsT->Release(); psT->Release(); sys->Release();
  dev->Release(); d3d->Release();
  SDL_DestroyWindow(win); SDL_Quit();
  std::printf("RESULT %s (%d failed checks)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}

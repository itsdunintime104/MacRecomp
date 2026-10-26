/* Proves the "Linking the game" recipe: a plain C file calls Direct3DCreate9 from the
 * DXVK install prefix through the C COM macros. No window; run with DXVK_WSI_DRIVER=SDL2.
 * The SDL2 WSI does not initialise SDL itself: SDL video must be up before Direct3DCreate9,
 * or SDL_Vulkan_GetInstanceExtensions dereferences a NULL video device (sdl2-compat on SDL3). */
#define COBJMACROS
#include <windows.h>
/* DXVK Native's windows_base.h defines THIS_ and THIS as empty in C too, so the C vtable
 * prototypes would lack the This argument. Restore the MinGW C definitions before d3d9.h;
 * d3d9.h defines INTERFACE before each interface it declares. */
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>
#include <SDL.h>
#include <stdio.h>

int main(void) {
  IDirect3D9 *d3d;
  D3DADAPTER_IDENTIFIER9 id;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("FAIL SDL_Init: %s\n", SDL_GetError()); return 1; }
  d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { printf("FAIL Direct3DCreate9 returned NULL\n"); return 1; }
  printf("adapters: %u\n", IDirect3D9_GetAdapterCount(d3d));
  if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id)))
    printf("adapter 0: %s\n", id.Description);
  IDirect3D9_Release(d3d);
  SDL_Quit();
  printf("PASS link_probe\n");
  return 0;
}

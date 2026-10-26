/* Win32 UI slice (runtime/src/win32_ui.c): the hooks other slices use. */
#ifndef WIN32_UI_H
#define WIN32_UI_H

#include <stdint.h>

/* The SDL_Window behind a guest HWND, or NULL for an unknown HWND or a child
 * window (only top-level windows have one). For the D3D9 backend only: the
 * window is then kept until exit, and DestroyWindow only hides it. */
void *w32_hwnd_to_sdl(uint32_t hwnd);

/* Translate pending SDL events into the Win32 message queue. Only the host
 * main thread pumps; elsewhere this does nothing. Never calls guest code. */
void w32_ui_pump(void);

/* Extra SDL_WindowFlags OR-ed into every top-level SDL_CreateWindow. The game
 * build sets SDL_WINDOW_VULKAN so DXVK can make a surface; tests leave it 0. */
void w32_ui_set_window_flags(uint32_t flags);

/* Start SDL video and events once (safe to call again). The D3D9 backend
 * calls it, because Direct3DCreate9 needs SDL video before any window exists. */
void w32_ui_init(void);
/* SetWindowPos(SWP_SHOWWINDOW) on the window whose SDL window is
 * host_window, if there is one: what Windows Direct3D 9 does to the device
 * window of a fullscreen device (Wine wined3d_device_setup_fullscreen_window). */
void w32_ui_show_device_window(void *host_window);

/* Nonzero while the game must not see this SDL scancode: Cmd, and the F of
 * a Ctrl+Cmd+F fullscreen toggle or a Cmd+F overlay toggle until it is
 * released. */
int w32_ui_key_hidden(int scancode);

#endif

/* d3dx9_41 shader imports: D3DXCompileShader on vkd3d-shader and
 * D3DXGetShaderConstantTable on a CTAB parser, with the guest ID3DXBuffer
 * and ID3DXConstantTable objects they return. Implemented in
 * runtime/src/d3dx_shader.c; which d3dx9_41 imports the game reaches is in
 * notes/d3dx_liveness.md.
 *
 * Both are stdcall import thunks (win32_thunk.h). They are not in any export
 * table here: the d3d slice lists them in win32_exports_d3d. */
#ifndef D3DX_GUEST_H
#define D3DX_GUEST_H

#include <stdint.h>

/* HRESULT D3DXCompileShader(LPCSTR pSrcData, UINT SrcDataLen,
 *     const D3DXMACRO *pDefines, LPD3DXINCLUDE pInclude, LPCSTR pFunctionName,
 *     LPCSTR pProfile, DWORD Flags, LPD3DXBUFFER *ppShader,
 *     LPD3DXBUFFER *ppErrorMsgs, LPD3DXCONSTANTTABLE *ppConstantTable)
 * 10 arguments, pops 40 bytes. */
void thunk_D3DXCompileShader(void);

/* HRESULT D3DXGetShaderConstantTable(const DWORD *pFunction,
 *     LPD3DXCONSTANTTABLE *ppConstantTable)
 * 2 arguments, pops 8 bytes. */
void thunk_D3DXGetShaderConstantTable(void);

/* A new guest ID3DXBuffer (refcount 1) holding a guest copy of size bytes
 * of host data; GetBufferPointer returns that copy and Release frees it.
 * Returns the guest object, or 0 when guest memory is full. For any D3DX
 * import that returns an ID3DXBuffer. */
uint32_t d3dx_buffer_new(const void *data, uint32_t size);

/* Number of D3DXCompileShader calls served from the disk cache
 * (~/Library/Caches/HarryPotterRecomp/shaders; HPRECOMP_SHADER_CACHE=0 turns
 * it off). */
uint32_t d3dx_shader_cache_hits(void);

/* Marks the game's CachedShaders stale (VERSION.DAT version field = 0) when
 * our compiler revision differs from the stamp beside it. save_dir is the
 * redirected save root (pe_boot make_save_dir). Returns the number of caches
 * found; 0 means the game has none yet. */
int d3dx_game_cache_check(const char *save_dir);

/* Writes our compiler revision beside each CachedShaders/VERSION.DAT, so the
 * next d3dx_game_cache_check keeps those caches. */
void d3dx_game_cache_stamp(const char *save_dir);

/* HRESULTs (d3dx9.h, d3d9.h). */
#define D3DXERR_INVALIDDATA 0x88760B59u /* MAKE_DDHRESULT(2905) */
#define D3DERR_INVALIDCALL 0x8876086Cu  /* MAKE_D3DHRESULT(2156) */

#endif

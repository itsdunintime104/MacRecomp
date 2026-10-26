/* Frame timing and slow host call log (HPRECOMP_FRAME_TRACE=1).
 *
 * Every COM method (com_guest.c vtables) and every import (pe_boot.c) runs
 * through a timed trampoline when the trace is on. Output goes to
 * HPRECOMP_FRAME_TRACE_OUT (default stderr), one line per event:
 *   F <n> <t_ms> <dt_ms> <cpu_ms> <host_ms> <max_ms> <max_name> <present_ms>
 *     at each IDirect3DDevice9::Present / IDirect3DSwapChain9::Present entry:
 *     dt is the time since the previous Present entry; cpu is the calling
 *     thread's CPU time over that interval; host is the time spent in
 *     outermost wrapped calls on that thread (the previous Present included,
 *     its own share is present_ms); max is the longest such call.
 *   S <t_ms> <tid> <thread_name> <name> <ms>
 *     any wrapped call on any thread that took 2 ms or more (t is its start).
 * Off (the default), ft_wrap and ft_wrap_method return host_func_va(fn), so
 * no call is timed. */
#ifndef HP_FRAME_TRACE_H
#define HP_FRAME_TRACE_H

#include <stdint.h>

#include "recomp_types.h"

int ft_enabled(void);
/* Guest address to call fn through a timed trampoline named name. */
uint32_t ft_wrap(recomp_func_t fn, const char *name);
/* As ft_wrap, named "<iface>#<index>"; marks the two Present methods. */
uint32_t ft_wrap_method(recomp_func_t fn, const char *iface, unsigned index);

#endif

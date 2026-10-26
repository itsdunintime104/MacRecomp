/* DirectSound 8 slice (runtime/src/win32_dsound.c): the two host-side hooks
 * the test needs. The game reaches the slice only through
 * DSOUND.dll!DirectSoundCreate8 (win32_exports_dsound). */
#ifndef WIN32_DSOUND_H
#define WIN32_DSOUND_H

#include <stdint.h>

/* When rate is not 0, the next DirectSoundCreate8 opens no audio device: the
 * mixer runs only when w32_dsound_render() pulls it, at rate frames per
 * second. 0 restores the default (FAudio on the host device). */
void w32_dsound_set_offline_rate(uint32_t rate);

/* Mix the next `frames` stereo float frames of the IDirectSound8 object
 * guest_ds into out (2 * frames floats, interleaved L R) and advance every
 * playing buffer, as the device thread does. */
void w32_dsound_render(uint32_t guest_ds, float *out, uint32_t frames);

#endif

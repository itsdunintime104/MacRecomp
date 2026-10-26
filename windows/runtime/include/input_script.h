/* Scripted input for unattended test runs (runtime/src/input_script.c). Off
 * unless HPRECOMP_INPUT_SCRIPT names a script file. Each line is
 * `<ms> <down|up|tap> <name>`; ms counts from the first DirectInput8Create
 * or XInputGetState. Names: DIK keys (RETURN ESCAPE SPACE UP DOWN LEFT RIGHT
 * A..Z 0..9 LSHIFT LCONTROL) or pad controls (PAD_A PAD_B PAD_X PAD_Y
 * PAD_START PAD_BACK PAD_UP PAD_DOWN PAD_LEFT PAD_RIGHT PAD_LX=<v> PAD_LY=<v>).
 * tap is down, then up 100 ms later. # starts a comment. */
#ifndef INPUT_SCRIPT_H
#define INPUT_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

/* Scripted XInput user 0 state. */
struct input_script_pad {
    uint16_t buttons; /* XINPUT_GAMEPAD_* bits */
    int16_t lx, ly;
};

/* Replaces the script with text and clears time zero and the held state.
 * Returns 0, or -1 with "line N: why" in err. The env loader and the tests
 * call it. */
int input_script_parse(const char *text, char *err, size_t err_size);

/* Milliseconds clock; NULL restores CLOCK_MONOTONIC. For tests. */
void input_script_set_clock(uint64_t (*now_ms)(void));

/* First DirectInput8Create or XInputGetState: loads the env script once
 * (a bad line is fatal) and records time zero once. */
void input_script_start(void);

/* ORs 0x80 into keys[dik] for each scripted key held now, then calls the
 * extra key source, if one is set. */
void input_script_keys(uint8_t keys[256]);

/* Sets a second key source called after the script on each keyboard poll
 * (a game file, such as a closed-loop input driver). NULL clears it. */
void input_script_set_extra(void (*fn)(uint8_t keys[256]));

/* DIK code of a script key name, or 0. */
uint16_t input_script_dik(const char *name);

/* 1 while a loaded script has pad lines; then fills *pad (if not NULL) with
 * the state now. 0 otherwise. Loads the env script once. */
int input_script_pad(struct input_script_pad *pad);

#endif

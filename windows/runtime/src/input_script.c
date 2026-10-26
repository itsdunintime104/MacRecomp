/* Scripted input (input_script.h): a time-ordered list of key and pad
 * events. win32_input.c ORs the keys into the DirectInput keyboard state and
 * reports the pad as XInput user 0. Each event is logged to stderr when it
 * first takes effect. */
#include "input_script.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAP_MS 100u

enum { EV_KEY, EV_BUTTON, EV_LX, EV_LY };

struct event {
    uint64_t ms;
    uint8_t kind;
    uint8_t down;
    uint16_t code;   /* DIK code or XINPUT_GAMEPAD_* bit */
    int16_t value;   /* stick value while down */
    uint32_t seq;    /* line order, to keep the sort stable */
    char name[24];
};

static struct {
    struct event *ev;
    size_t n, next; /* next: first event not yet applied */
    int has_pad, started;
    uint64_t t0;
    uint8_t keys[256];
    struct input_script_pad pad;
} s;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static uint64_t (*s_now)(void) = mono_ms;

static const struct {
    const char *name;
    uint16_t code;
} KEYS[] = {
    {"RETURN", 0x1C}, {"ESCAPE", 0x01}, {"SPACE", 0x39}, {"UP", 0xC8},
    {"DOWN", 0xD0},   {"LEFT", 0xCB},   {"RIGHT", 0xCD}, {"LSHIFT", 0x2A},
    {"LCONTROL", 0x1D},
};
static const struct {
    const char *name;
    uint16_t bit;
} BUTTONS[] = {
    {"PAD_UP", 0x0001},    {"PAD_DOWN", 0x0002}, {"PAD_LEFT", 0x0004},
    {"PAD_RIGHT", 0x0008}, {"PAD_START", 0x0010}, {"PAD_BACK", 0x0020},
    {"PAD_A", 0x1000},     {"PAD_B", 0x2000},    {"PAD_X", 0x4000},
    {"PAD_Y", 0x8000},
};

/* DIK code of a key name, or 0. Letters and digits by keyboard row. */
uint16_t input_script_dik(const char *n)
{
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strcmp(n, KEYS[i].name) == 0)
            return KEYS[i].code;
    if (n[0] == 0 || n[1] != 0)
        return 0;
    static const struct {
        const char *row;
        uint16_t first;
    } ROWS[] = {{"1234567890", 0x02}, {"QWERTYUIOP", 0x10}, {"ASDFGHJKL", 0x1E}, {"ZXCVBNM", 0x2C}};
    for (size_t r = 0; r < sizeof ROWS / sizeof ROWS[0]; r++) {
        const char *p = strchr(ROWS[r].row, n[0]);
        if (p)
            return (uint16_t)(ROWS[r].first + (p - ROWS[r].row));
    }
    return 0;
}

/* Fills e from a control name. Returns 0, or -1 with why. */
static int name_parse(const char *n, struct event *e, const char **why)
{
    if (strlen(n) >= sizeof e->name) {
        *why = "name too long";
        return -1;
    }
    strcpy(e->name, n);
    uint16_t dik = input_script_dik(n);
    if (dik) {
        e->kind = EV_KEY, e->code = dik;
        return 0;
    }
    for (size_t i = 0; i < sizeof BUTTONS / sizeof BUTTONS[0]; i++)
        if (strcmp(n, BUTTONS[i].name) == 0) {
            e->kind = EV_BUTTON, e->code = BUTTONS[i].bit;
            return 0;
        }
    if (strncmp(n, "PAD_LX=", 7) == 0 || strncmp(n, "PAD_LY=", 7) == 0) {
        char *end;
        long v = strtol(n + 7, &end, 10);
        if (end == n + 7 || *end || v < -32768 || v > 32767) {
            *why = "stick value not in -32768..32767";
            return -1;
        }
        e->kind = n[5] == 'X' ? EV_LX : EV_LY, e->value = (int16_t)v;
        return 0;
    }
    *why = "unknown name";
    return -1;
}

/* By time, then by line order. */
static int ev_cmp(const void *a, const void *b)
{
    const struct event *x = a, *y = b;
    if (x->ms != y->ms)
        return x->ms < y->ms ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

int input_script_parse(const char *text, char *err, size_t err_size)
{
    struct event *ev = NULL;
    size_t n = 0, cap = 0;
    int has_pad = 0, line_no = 0;
    const char *why = NULL;
    for (const char *p = text; *p && !why;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        line_no++;
        if (len >= sizeof line) {
            why = "line too long";
            break;
        }
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (eol != NULL);
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char ms_s[32], act[16], name[64], extra[2];
        int got = sscanf(line, "%31s %15s %63s %1s", ms_s, act, name, extra);
        if (got <= 0)
            continue; /* blank or comment */
        if (got != 3) {
            why = "want <ms> <down|up|tap> <name>";
            break;
        }
        char *end;
        unsigned long long ms = strtoull(ms_s, &end, 10);
        if (!isdigit((unsigned char)ms_s[0]) || *end) {
            why = "bad ms";
            break;
        }
        int down = strcmp(act, "down") == 0, up = strcmp(act, "up") == 0,
            tap = strcmp(act, "tap") == 0;
        if (!down && !up && !tap) {
            why = "action is not down, up or tap";
            break;
        }
        struct event e = {.ms = ms, .down = (uint8_t)!up};
        if (name_parse(name, &e, &why) != 0)
            break;
        has_pad |= e.kind != EV_KEY;
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 16;
            ev = realloc(ev, cap * sizeof *ev);
        }
        e.seq = (uint32_t)n;
        ev[n++] = e;
        if (tap) {
            e.ms += TAP_MS, e.down = 0, e.seq = (uint32_t)n;
            ev[n++] = e;
        }
    }
    if (why) {
        snprintf(err, err_size, "line %d: %s", line_no, why);
        free(ev);
        return -1;
    }
    if (n)
        qsort(ev, n, sizeof *ev, ev_cmp);
    pthread_mutex_lock(&s_lock);
    free(s.ev);
    memset(&s, 0, sizeof s);
    s.ev = ev, s.n = n, s.has_pad = has_pad;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void input_script_set_clock(uint64_t (*now_ms)(void))
{
    s_now = now_ms ? now_ms : mono_ms;
}

/* Loads HPRECOMP_INPUT_SCRIPT; runs once, without s_lock held. */
static void load_env_once(void)
{
    const char *path = getenv("HPRECOMP_INPUT_SCRIPT");
    if (!path || !path[0])
        return;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "input_script: cannot open %s\n", path);
        abort();
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *text = malloc((size_t)size + 1);
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;
    char err[128];
    if (input_script_parse(text, err, sizeof err) != 0) {
        fprintf(stderr, "input_script: %s: %s\n", path, err);
        abort();
    }
    free(text);
    fprintf(stderr, "input_script: %s loaded\n", path);
}

static pthread_once_t s_env_once = PTHREAD_ONCE_INIT;
static void load_env(void)
{
    pthread_once(&s_env_once, load_env_once);
}

void input_script_start(void)
{
    load_env();
    pthread_mutex_lock(&s_lock);
    if (!s.started) {
        s.started = 1;
        s.t0 = s_now();
    }
    pthread_mutex_unlock(&s_lock);
}

/* Runs with s_lock held: applies and logs every event due by now. */
static void advance(void)
{
    if (!s.started)
        return;
    uint64_t t = s_now() - s.t0;
    for (; s.next < s.n && s.ev[s.next].ms <= t; s.next++) {
        const struct event *e = &s.ev[s.next];
        switch (e->kind) {
        case EV_KEY: s.keys[e->code] = e->down; break;
        case EV_BUTTON:
            s.pad.buttons = e->down ? (uint16_t)(s.pad.buttons | e->code)
                                    : (uint16_t)(s.pad.buttons & ~e->code);
            break;
        case EV_LX: s.pad.lx = e->down ? e->value : 0; break;
        case EV_LY: s.pad.ly = e->down ? e->value : 0; break;
        }
        fprintf(stderr, "INPUT t=%llu %s %s\n", (unsigned long long)t,
                e->down ? "down" : "up", e->name);
    }
}

static void (*s_extra)(uint8_t keys[256]);

void input_script_set_extra(void (*fn)(uint8_t keys[256]))
{
    s_extra = fn;
}

void input_script_keys(uint8_t keys[256])
{
    pthread_mutex_lock(&s_lock);
    advance();
    for (int i = 0; i < 256; i++)
        if (s.keys[i])
            keys[i] |= 0x80;
    pthread_mutex_unlock(&s_lock);
    if (s_extra)
        s_extra(keys);
}

int input_script_pad(struct input_script_pad *pad)
{
    load_env();
    pthread_mutex_lock(&s_lock);
    int has = s.has_pad;
    if (has && pad) {
        advance();
        *pad = s.pad;
    }
    pthread_mutex_unlock(&s_lock);
    return has;
}

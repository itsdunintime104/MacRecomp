/* Frame trace test (frame_trace.c): with HPRECOMP_FRAME_TRACE=1, a call through
 * a wrapped import that takes >= 2 ms writes one S line, a fast call writes
 * none, and each IDirect3DDevice9::Present entry after the first writes an F
 * line whose dt, host time and longest call cover the interval. With the trace
 * off, ft_wrap returns the function's own address. Links frame_trace.c and the
 * engine's guest_mem.c, host_funcs.c and recomp_glue.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "frame_trace.h"

recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

static int g_fast_calls, g_present_calls;
static void fast_fn(void) { g_fast_calls++; }
static void slow_fn(void)
{
    struct timespec ts = {0, 5000000}; /* 5 ms */
    nanosleep(&ts, NULL);
}
static void present_fn(void) { g_present_calls++; }

int main(void)
{
    if (guest_mem_init() != 0) {
        fprintf(stderr, "guest memory init failed\n");
        return 1;
    }
    char path[] = "/tmp/hp_ft_testXXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    /* Off: no trampoline. ft_enabled caches its first answer, so the off
     * case runs in a child process. */
    setenv("HPRECOMP_FRAME_TRACE", "0", 1);
    pid_t pid = fork();
    if (pid == 0)
        _exit(ft_wrap(fast_fn, "k!Fast") == host_func_va(fast_fn) ? 0 : 1);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    setenv("HPRECOMP_FRAME_TRACE", "1", 1);
    setenv("HPRECOMP_FRAME_TRACE_OUT", path, 1);
    uint32_t fast = ft_wrap(fast_fn, "k!Fast"), slow = ft_wrap(slow_fn, "k!Slow");
    uint32_t present = ft_wrap_method(present_fn, "IDirect3DDevice9", 17);
    CHECK(fast != host_func_va(fast_fn));
    CHECK(ft_wrap(fast_fn, "k!Again") == fast); /* one slot per function */
    guest_resolve(present)();
    guest_resolve(fast)();
    guest_resolve(slow)();
    guest_resolve(present)();
    CHECK(g_fast_calls == 1 && g_present_calls == 2);

    char buf[4096];
    fflush(NULL); /* frame_trace flushes once a second; the test runs in less */
    FILE *f = fopen(path, "r");
    size_t n = f != NULL ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = '\0';
    if (f != NULL)
        fclose(f);
    unlink(path);

    int s_lines = 0, f_lines = 0;
    double dt = 0, host = 0, max = 0;
    char max_name[64] = "";
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (line[0] == 'S') {
            s_lines++;
            CHECK(strstr(line, " k!Slow ") != NULL);
        } else if (line[0] == 'F') {
            f_lines++;
            unsigned long long fn;
            double t, cpu, pres;
            CHECK(sscanf(line, "F %llu %lf %lf %lf %lf %lf %63s %lf", &fn, &t, &dt, &cpu, &host,
                         &max, max_name, &pres) == 8);
        }
    }
    CHECK(s_lines == 1);
    CHECK(f_lines == 1);
    CHECK(dt >= 5.0 && host >= 5.0 && host <= dt);
    CHECK(max >= 5.0 && strcmp(max_name, "k!Slow") == 0);
    if (g_fail)
        fprintf(stderr, "trace was:\n%s\n", buf);
    printf("%s test_frame_trace\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}

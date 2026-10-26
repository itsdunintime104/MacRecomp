/* main() for the boot driver: fatal-signal reporting, then pe_boot_run().
 * A fatal signal prints the signal, the fault address, the guest stack
 * pointer and return address, the last indirect-call targets and the host
 * backtrace (lifted functions are named sub_<guest VA>), then exits with 2,
 * so no macOS crash dialog appears. Usage: <game binary> IMAGE GAME_DIR (both required). */
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recomp_types.h"
#include "diag.h"
#include "guest_mem.h"

int pe_boot_run(const char *exe_path, const char *game_dir);

static void on_fatal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    char buf[256];
    /* Only the mapped stack ranges are read (thread stacks, callback stacks,
     * main stack); the handler must not fault again. */
    uint32_t ret = 0;
    if (g_esp >= 0xE0000000u && g_esp < 0xFDFFFFFCu)
        memcpy(&ret, guest_ptr(g_esp), 4);
    int n = snprintf(buf, sizeof buf,
                     "\nFATAL signal %d (%s) addr %p guest_off 0x%llx g_esp 0x%08X [esp] 0x%08X "
                     "fs 0x%08X\n",
                     sig, strsignal(sig), si->si_addr,
                     (unsigned long long)((uintptr_t)si->si_addr - (uintptr_t)g_guest_base),
                     g_esp, ret, g_fs_base);
    write(2, buf, (size_t)n);
    recomp_icall_tail(buf, sizeof buf, 8);
    write(2, "icalls: ", 8);
    write(2, buf, strlen(buf));
    write(2, "\n", 1);
    void *frames[64];
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, 2);
    _exit(2);
}

static void install_fatal_handlers(void)
{
    static char altstack[1 << 16];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE GAME_DIR\n", argv[0]);
        return 2;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    install_fatal_handlers();
    return pe_boot_run(argv[1], argv[2]);
}

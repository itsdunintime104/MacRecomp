/* Forked test children that abort on purpose. A handler turns SIGABRT into a
 * plain exit with status 128 + SIGABRT, so macOS ReportCrash writes no crash
 * report (and shows no dialog) for an abort the test expects. */
#ifndef QUIET_ABORT_H
#define QUIET_ABORT_H

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void quiet_abort_handler(int sig) { _exit(128 + sig); }

/* Call first thing in the child after fork(). */
static void quiet_abort_child(void) { signal(SIGABRT, quiet_abort_handler); }

/* True when the child ended through abort(). */
#define CHILD_ABORTED(st) (WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGABRT)

#endif

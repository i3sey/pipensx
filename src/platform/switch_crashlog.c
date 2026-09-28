#include "switch_crashlog.h"

#include "core/util.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* volatile: also read by the watchdog thread (ANR dumps). Pointer-sized
 * aligned loads/stores are atomic on the targets we build for. */
static const char *volatile g_stage = "before main";

static void fatal_signal(int signal_number) {
    char message[256];
    snprintf(message, sizeof(message),
             "[crash] signal=%d stage=%s\n",
             signal_number, g_stage ? g_stage : "unknown");
    log_emergency(message);
    _exit(128 + signal_number);
}

void switch_crashlog_install(void) {
    signal(SIGABRT, fatal_signal);
    signal(SIGFPE, fatal_signal);
    signal(SIGILL, fatal_signal);
    signal(SIGSEGV, fatal_signal);
#ifdef SIGBUS
    signal(SIGBUS, fatal_signal);
#endif
}

void switch_crashlog_stage(const char *stage) {
    g_stage = stage;
}

const char *switch_crashlog_last_stage(void) {
    return g_stage;
}

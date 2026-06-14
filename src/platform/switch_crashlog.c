#include "switch_crashlog.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CRASH_LOG_PATH "sdmc:/switch/pipensx/pipensx.log"

static const char *g_stage = "before main";

static void append_text(const char *text) {
    int fd = open(CRASH_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    write(fd, text, strlen(text));
    close(fd);
}

static void fatal_signal(int signal_number) {
    char message[256];
    snprintf(message, sizeof(message),
             "[crash] signal=%d stage=%s\n",
             signal_number, g_stage ? g_stage : "unknown");
    append_text(message);
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

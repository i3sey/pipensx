#include "switch_crashlog.h"

#include "core/util.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __SWITCH__
#  include <switch.h>
#endif

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

#ifdef __SWITCH__

/* POSIX signal handlers are dead code on a Switch: newlib's raise() falls
 * through to the _kill_r stub, which is a hard-coded ENOSYS (verified by
 * disassembling the shipped NRO). A SIGSEGV/SIGABRT-class fault therefore
 * kills the process with nothing in pipensx.log — exactly what issue #86
 * showed. libnx instead routes CPU faults (data/instruction abort,
 * misaligned PC/SP, bad SVC, ...) through this hook: a weak symbol the
 * app may define. It runs before the process dies through hbloader's
 * fatal (the Atmosphere "std::abort (0xFFE)" screen the reporter saw),
 * so the fault addresses reach the log and the next bug report carries
 * them. Symbolize against the same build's pipensx.elf: subtract the
 * load slide, computed as (handler runtime address logged here) minus
 * (its link address from `nm pipensx.elf | grep __libnx_exception_handler`).
 *
 * Async-signal-safety: the heap may be the thing that is corrupted, so
 * this must not run stdio or allocate. Raw write() through log_emergency
 * (the fd log_init() owns) with a hand-rolled hex formatter. The buffer
 * lives on the faulting thread's stack; a stack-overflow fault may still
 * double-fault and die with the plain panic — no worse than before. */
void __libnx_exception_handler(ThreadExceptionDump *ctx);

static void append_hex(char *buf, size_t cap, size_t *off, const char *name,
                       u64 value) {
    static const char hex[] = "0123456789abcdef";
    while (*name && *off + 1 < cap)
        buf[(*off)++] = *name++;
    if (*off + 1 < cap)
        buf[(*off)++] = '=';
    for (int i = 15; i >= 0 && *off < cap; --i)
        buf[(*off)++] = hex[(value >> (i * 4)) & 0xf];
    if (*off < cap)
        buf[(*off)++] = ' ';
}

static void append_text(char *buf, size_t cap, size_t *off, const char *name,
                        const char *text) {
    while (*name && *off + 1 < cap)
        buf[(*off)++] = *name++;
    if (*off + 1 < cap)
        buf[(*off)++] = '=';
    /* g_stage always points at a string literal (switch_crashlog_stage
       call sites), so a bounded read is safe. */
    while (*text && *off + 1 < cap)
        buf[(*off)++] = *text++;
    if (*off < cap)
        buf[(*off)++] = ' ';
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    if (!ctx)
        return;
    char buf[256];
    size_t off = 0;
    static const char prefix[] = "[fault] ";
    for (size_t i = 0; i < sizeof(prefix) - 1 && off < sizeof(buf); ++i)
        buf[off++] = prefix[i];
    append_hex(buf, sizeof(buf), &off, "desc", ctx->error_desc);
    append_hex(buf, sizeof(buf), &off, "pc", ctx->pc.x);
    append_hex(buf, sizeof(buf), &off, "lr", ctx->lr.x);
    append_hex(buf, sizeof(buf), &off, "sp", ctx->sp.x);
    append_hex(buf, sizeof(buf), &off, "far", ctx->far.x);
    append_text(buf, sizeof(buf), &off, "stage",
                g_stage ? g_stage : "?");
    append_hex(buf, sizeof(buf), &off, "handler",
               (u64)(uintptr_t)&__libnx_exception_handler);
    if (off + 1 < sizeof(buf)) {
        buf[off++] = '\n';
        buf[off] = '\0';
    } else {
        buf[sizeof(buf) - 1] = '\0';
    }
    log_emergency(buf);
}

#endif /* __SWITCH__ */

#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SWITCH__
#  include <switch.h>
#  define CLOCK_ID CLOCK_MONOTONIC
#else
#  include <time.h>
#  define CLOCK_ID CLOCK_MONOTONIC
#endif

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000u +
           (uint64_t)(ts.tv_nsec / 1000u);
}

time_t now_sec(void) {
    return (time_t)(now_ms() / 1000u);
}

static FILE *g_logfile = NULL;
static uint64_t g_log_flush_ms = 0;
static atomic_int g_telemetry_enabled = 0;
static atomic_uint g_telemetry_generation = 1;

#define LOG_ROTATE_BYTES (32ULL * 1024ULL * 1024ULL)

static void rotate_log_if_needed(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0 || st.st_size < 0 ||
        (uint64_t)st.st_size < LOG_ROTATE_BYTES)
        return;
    char backup[1024];
    int n = snprintf(backup, sizeof(backup), "%s.1", path);
    if (n <= 0 || (size_t)n >= sizeof(backup))
        return;
    remove(backup);
    rename(path, backup);
}

void log_init(const char *path) {
    if (!path) return;
    log_close();
    rotate_log_if_needed(path);
    g_logfile = fopen(path, "a");
    if (g_logfile) {
        setvbuf(g_logfile, NULL, _IOFBF, 64 * 1024);
        fprintf(g_logfile, "=== pipensx log started ===\n");
        fflush(g_logfile);
        g_log_flush_ms = now_ms();
    }
}

void log_close(void) {
    if (!g_logfile) return;
    fflush(g_logfile);
    fclose(g_logfile);
    g_logfile = NULL;
}

void log_flush(void) {
    if (!g_logfile) return;
    flockfile(g_logfile);
    fflush(g_logfile);
    g_log_flush_ms = now_ms();
    funlockfile(g_logfile);
}

int log_clear(void) {
    if (!g_logfile) return 0;
    flockfile(g_logfile);
    int fd = fileno(g_logfile);
    int ok = fflush(g_logfile) == 0 && fd >= 0 && ftruncate(fd, 0) == 0;
    if (ok) {
        rewind(g_logfile);
        ok = fprintf(g_logfile, "=== pipensx log cleared ===\n") > 0 &&
             fflush(g_logfile) == 0;
        g_log_flush_ms = now_ms();
    }
    funlockfile(g_logfile);
    return ok;
}

void log_msg(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);

#ifndef __SWITCH__
    vprintf(fmt, ap);
    fflush(stdout);
#endif

    if (g_logfile) {
        flockfile(g_logfile);
        /* Prefix every line with elapsed ms */
        struct timespec ts;
        clock_gettime(CLOCK_ID, &ts);
        uint64_t ms = (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
        fprintf(g_logfile, "[%7llu] ", (unsigned long long)ms % 10000000ULL);
        vfprintf(g_logfile, fmt, ap2);
        if (ms - g_log_flush_ms >= 1000) {
            fflush(g_logfile);
            g_log_flush_ms = ms;
        }
        funlockfile(g_logfile);
    }

    va_end(ap2);
    va_end(ap);
}

void telemetry_set_enabled(int enabled) {
    int next = enabled ? 1 : 0;
    int previous = atomic_exchange_explicit(
        &g_telemetry_enabled, next, memory_order_acq_rel);
    if (previous != next) {
        uint32_t generation = atomic_fetch_add_explicit(
            &g_telemetry_generation, 1, memory_order_acq_rel) + 1;
        log_msg("[telemetry] schema=1 stage=control enabled=%d generation=%u\n",
                next, generation);
    }
}

int telemetry_enabled(void) {
    return atomic_load_explicit(&g_telemetry_enabled, memory_order_acquire);
}

uint32_t telemetry_generation(void) {
    return atomic_load_explicit(&g_telemetry_generation,
                                memory_order_acquire);
}

void telemetry_log(const char *stage, const char *tag, const char *fmt, ...) {
    if (!telemetry_enabled())
        return;
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
    va_end(ap);
    log_msg("[telemetry] schema=1 stage=%s tag=%s %s\n",
            stage ? stage : "unknown", tag && tag[0] ? tag : "-", body);
}

static void diagnostic_log(const char *level, const char *stage,
                           const char *tag, const char *fmt, va_list ap) {
    char body[1024];
    vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
    for (char *cursor = body; *cursor; ++cursor) {
        if (*cursor == '\n' || *cursor == '\r' || *cursor == '\t')
            *cursor = ' ';
        else if (*cursor == '\'' || *cursor == '"' || *cursor == '\\')
            *cursor = '_';
    }
    log_msg("[diagnostic] schema=1 level=%s stage=%s tag=%s %s\n",
            level ? level : "info", stage ? stage : "unknown",
            tag && tag[0] ? tag : "-", body);
    log_flush();
}

void diagnostic_error(const char *stage, const char *tag,
                      const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diagnostic_log("error", stage, tag, fmt, ap);
    va_end(ap);
}

void diagnostic_snapshot(const char *stage, const char *tag,
                         const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diagnostic_log("snapshot", stage, tag, fmt, ap);
    va_end(ap);
}

void fmt_bytes(char *buf, size_t len, uint64_t b) {
    if      (b >= 1024ULL*1024*1024) snprintf(buf, len, "%.2f GB", b/(1024.0*1024*1024));
    else if (b >= 1024ULL*1024)      snprintf(buf, len, "%.2f MB", b/(1024.0*1024));
    else if (b >= 1024ULL)           snprintf(buf, len, "%.2f KB", b/1024.0);
    else                             snprintf(buf, len, "%llu B",  (unsigned long long)b);
}

void fmt_speed(char *buf, size_t len, uint64_t bps) {
    if      (bps >= 1024ULL*1024) snprintf(buf, len, "%.1f MB/s", bps/(1024.0*1024));
    else if (bps >= 1024ULL)      snprintf(buf, len, "%.1f KB/s", bps/1024.0);
    else                          snprintf(buf, len, "%llu B/s", (unsigned long long)bps);
}

void hex20(char buf[41], const uint8_t hash[20]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        buf[i*2]   = h[hash[i]>>4];
        buf[i*2+1] = h[hash[i]&15];
    }
    buf[40] = 0;
}

void rand_bytes(uint8_t *buf, size_t n) {
#ifdef __SWITCH__
    /* libnx provides randomGet() */
    extern void randomGet(void *buf, size_t len);
    randomGet(buf, n);
#else
    /* PC: use /dev/urandom */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { fread(buf, 1, n, f); fclose(f); }
    else { for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rand() >> 3); }
#endif
}

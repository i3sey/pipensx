#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

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

time_t now_sec(void) {
    return (time_t)(now_ms() / 1000u);
}

static FILE *g_logfile = NULL;

void log_init(const char *path) {
    if (!path) return;
    log_close();
    g_logfile = fopen(path, "w");
    if (g_logfile) {
        fprintf(g_logfile, "=== pipensx log started ===\n");
        fflush(g_logfile);
    }
}

void log_close(void) {
    if (!g_logfile) return;
    fflush(g_logfile);
    fclose(g_logfile);
    g_logfile = NULL;
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
        /* Prefix every line with elapsed ms */
        struct timespec ts;
        clock_gettime(CLOCK_ID, &ts);
        uint64_t ms = (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
        fprintf(g_logfile, "[%7llu] ", (unsigned long long)ms % 10000000ULL);
        vfprintf(g_logfile, fmt, ap2);
        fflush(g_logfile);
    }

    va_end(ap2);
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

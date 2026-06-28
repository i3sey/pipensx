#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Monotonic time in milliseconds */
uint64_t now_ms(void);
/* Monotonic time in microseconds, for aggregate performance timing. */
uint64_t now_us(void);
/* Wall-clock seconds (for DHT) */
time_t   now_sec(void);

/* Open log file (call once at startup, path = NULL to disable) */
void log_init(const char *path);
void log_close(void);
void log_flush(void);
int log_clear(void);

/* Logging — file-only on Switch, stdout and file on PC. */
void log_msg(const char *fmt, ...);

/* Optional rate-limited throughput telemetry. Disabled by default. */
void telemetry_set_enabled(int enabled);
int telemetry_enabled(void);
uint32_t telemetry_generation(void);
void telemetry_log(const char *stage, const char *tag, const char *fmt, ...);

/* Structured problem records. Errors and explicit snapshots are always kept. */
void diagnostic_error(const char *stage, const char *tag,
                      const char *fmt, ...);
void diagnostic_snapshot(const char *stage, const char *tag,
                         const char *fmt, ...);

/* Format bytes as "1.23 MB" etc. into buf (len >= 16) */
void fmt_bytes(char *buf, size_t len, uint64_t bytes);
/* Format speed as "234.5 KB/s" into buf (len >= 16) */
void fmt_speed(char *buf, size_t len, uint64_t bytes_per_sec);

/* Hex encode 20-byte hash into buf[41] */
void hex20(char buf[41], const uint8_t hash[20]);

/* Random bytes — implemented differently per platform */
void rand_bytes(uint8_t *buf, size_t n);

/* Bitfield helpers (pieces/have bitfield: MSB first per BT spec) */
static inline int bf_has(const uint8_t *bf, uint32_t idx) {
    return (bf[idx/8] >> (7 - idx%8)) & 1;
}
static inline void bf_set(uint8_t *bf, uint32_t idx) {
    bf[idx/8] |= (1u << (7 - idx%8));
}

/*
 * Probe telemetry: the sample ring, the event log and the ini parser.
 *
 * Deliberately free of libnx so the same code builds on the PC and is covered
 * by tests/test_probe_report.c. Everything that touches a Switch service lives
 * in probe_main.c.
 *
 * The ring exists because the whole point of the probe is the window where the
 * console is asleep: fs may be unavailable there, so samples are collected in
 * RAM and only flushed once we are awake again.
 */
#ifndef PIPENSX_PROBE_REPORT_H
#define PIPENSX_PROBE_REPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 1 Hz for half an hour — the experiments in the plan run 10 minutes each, and
 * the whole ring has to render into one static report buffer.
 * 4 net threads is the swarm stand-in.
 */
#define PROBE_RING_CAPACITY 1800
#define PROBE_NET_THREADS   4

/* Sample flags. */
#define PROBE_FLAG_HALF_AWAKE  0x0001u /* bgtc:t IsInHalfAwake returned true */
#define PROBE_FLAG_NET_UP      0x0002u /* nifm reports a connection */
#define PROBE_FLAG_KEPT_SLEEP  0x0004u /* SetKeptInSleep was accepted */
#define PROBE_FLAG_POST_WAKE   0x0008u /* first sample after a psc wake */
#define PROBE_FLAG_SOCK_KEPT   0x0010u /* socket 0 registered with the request */

typedef struct {
    uint64_t tick;                        /* armGetSystemTick() */
    uint64_t rtc;                         /* wall clock, seconds */
    uint64_t heap_used;                   /* svcGetInfo(UsedMemorySize) */
    uint64_t rx_bytes[PROBE_NET_THREADS]; /* per download thread, cumulative */
    uint64_t sd_bytes;                    /* written to SD, cumulative (E7) */
    uint32_t flags;
    uint8_t  psc_state;                   /* last PscPmState seen */
    uint8_t  sock_alive;                  /* bitmask: thread's socket is open */
} probe_sample_t;

typedef struct {
    probe_sample_t samples[PROBE_RING_CAPACITY];
    size_t   next;  /* write cursor */
    uint64_t total; /* samples ever pushed; > capacity means it wrapped */
} probe_ring_t;

void probe_ring_reset(probe_ring_t *ring);
void probe_ring_push(probe_ring_t *ring, const probe_sample_t *sample);
size_t probe_ring_count(const probe_ring_t *ring);
/* index 0 is the oldest sample still held. NULL when out of range. */
const probe_sample_t *probe_ring_at(const probe_ring_t *ring, size_t index);

/*
 * Event log: init results, psc transitions, bgtc result codes.
 *
 * A line ring, not append-only: a multi-day run logs one line per sleep window,
 * and there the tail is the whole point (the question is when windows stop
 * being granted). Oldest lines are dropped to make room — except the prefix
 * pinned by probe_log_pin(), which is how the startup result codes survive a
 * day of eviction.
 */
#define PROBE_LOG_CAPACITY 16384

typedef struct {
    char     text[PROBE_LOG_CAPACITY];
    size_t   len;
    size_t   pinned;  /* bytes at the front that eviction must not touch */
    unsigned evicted; /* lines dropped from the front */
    int      truncated;
} probe_log_t;

void probe_log_reset(probe_log_t *log);
void probe_log_add(probe_log_t *log, const char *line);
void probe_log_addf(probe_log_t *log, const char *fmt, ...);
/* Everything logged so far becomes immune to eviction. */
void probe_log_pin(probe_log_t *log);

/*
 * Renders log + samples as text. Returns the number of bytes written (never
 * more than out_size - 1); the output is always NUL-terminated and truncated
 * on a line boundary.
 */
size_t probe_report_format(const probe_ring_t *ring, const probe_log_t *log,
                           char *out, size_t out_size);

typedef struct {
    char     host[128];   /* name or dotted-quad of the plain-HTTP load source */
    char     path[256];
    uint16_t port;        /* load source port */
    uint16_t report_port; /* our own TCP report server */
    int      net_threads; /* 0 disables the network load entirely */
    int      enable_psc;
    int      enable_bgtc;
    /* The three bgtc requests, separately, so a run can tell which of them (if
     * any) is what opens half-awake windows. bgtc_schedule is the interval in
     * seconds asked of ScheduleTask; 0 does not call it at all. */
    int      bgtc_stay;
    int      bgtc_task;
    int      bgtc_schedule;
    int      bgtc_scan_range; /* sweep command ids when the documented ones fail */
    int      enable_kept_in_sleep; /* nifmRequestSetKeptInSleep on the request */
    int      enable_kept_socket;   /* register socket 0 for WoWLAN keep-alive */
    /* Download only inside half-awake windows. For runs spanning a normal day,
     * where line-rate traffic through every waking hour measures nothing. */
    int      load_in_sleep_only;
    int      enable_loopback_check;
    int      enable_sd_writer; /* E7: write to SD while asleep */
    int      run_seconds; /* 0 = until terminated */
} probe_config_t;

void probe_config_defaults(probe_config_t *cfg);
/* key=value lines, '#' and ';' start a comment. Unknown keys are ignored. */
void probe_config_parse(probe_config_t *cfg, const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PIPENSX_PROBE_REPORT_H */

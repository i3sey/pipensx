/*
 * The probe collects its samples in a RAM ring because the console is asleep
 * for the measurement that matters. If the ring loses or reorders samples
 * across its wrap, the answer to "did homebrew threads get CPU while asleep"
 * is unreadable — so that is what this checks.
 */
#include "../src/probe/probe_report.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static probe_ring_t g_ring;
static probe_log_t g_log;
static char g_out[512 * 1024];

static void push_seq(probe_ring_t *ring, uint64_t first, uint64_t count)
{
    uint64_t i;
    for (i = 0; i < count; i++) {
        probe_sample_t s;
        memset(&s, 0, sizeof(s));
        s.tick = first + i;
        s.rtc = 1000 + first + i;
        s.rx_bytes[0] = (first + i) * 10;
        probe_ring_push(ring, &s);
    }
}

static void test_ring_below_capacity(void)
{
    probe_ring_reset(&g_ring);
    push_seq(&g_ring, 0, 10);

    assert(probe_ring_count(&g_ring) == 10);
    assert(probe_ring_at(&g_ring, 0)->tick == 0);
    assert(probe_ring_at(&g_ring, 9)->tick == 9);
    assert(probe_ring_at(&g_ring, 10) == NULL);
}

static void test_ring_wrap_keeps_order_and_newest(void)
{
    size_t i;
    const uint64_t extra = 250;

    probe_ring_reset(&g_ring);
    push_seq(&g_ring, 0, PROBE_RING_CAPACITY + extra);

    /* Full, and the oldest survivor is the one right after the overwrite. */
    assert(probe_ring_count(&g_ring) == PROBE_RING_CAPACITY);
    assert(probe_ring_at(&g_ring, 0)->tick == extra);
    assert(probe_ring_at(&g_ring, PROBE_RING_CAPACITY - 1)->tick ==
           PROBE_RING_CAPACITY + extra - 1);

    /* Strictly increasing across the wrap: no gap, no reorder. */
    for (i = 1; i < PROBE_RING_CAPACITY; i++) {
        const probe_sample_t *prev = probe_ring_at(&g_ring, i - 1);
        const probe_sample_t *cur = probe_ring_at(&g_ring, i);
        assert(cur->tick == prev->tick + 1);
        assert(cur->rtc == prev->rtc + 1);
    }
}

static void test_report_holds_first_and_last(void)
{
    size_t len;
    char expect[64];
    const uint64_t last = PROBE_RING_CAPACITY + 4;

    probe_ring_reset(&g_ring);
    probe_log_reset(&g_log);
    probe_log_add(&g_log, "init bsd rc=0");
    probe_log_addf(&g_log, "psc state=%d", 2);
    push_seq(&g_ring, 0, PROBE_RING_CAPACITY + 5);

    len = probe_report_format(&g_ring, &g_log, g_out, sizeof(g_out));
    assert(len > 0 && len < sizeof(g_out));
    assert(g_out[len] == '\0');
    assert(strstr(g_out, "init bsd rc=0") != NULL);
    assert(strstr(g_out, "psc state=2") != NULL);
    /* Oldest held sample is tick 5, newest is the very last pushed. */
    assert(strstr(g_out, "\n5,1005,") != NULL);
    snprintf(expect, sizeof(expect), "\n%llu,%llu,", (unsigned long long)last,
             (unsigned long long)(1000 + last));
    assert(strstr(g_out, expect) != NULL);
    assert(strstr(g_out, "truncated") == NULL);
}

static void test_report_truncates_on_a_line_boundary(void)
{
    char small[300];
    size_t len;

    probe_ring_reset(&g_ring);
    probe_log_reset(&g_log);
    push_seq(&g_ring, 0, 100);

    len = probe_report_format(&g_ring, &g_log, small, sizeof(small));
    assert(len < sizeof(small));
    assert(small[len] == '\0');
    assert(len == 0 || small[len - 1] == '\n');
}

/* A day-long run logs a line per sleep window: the tail must survive, and the
 * pinned startup lines with it. */
static void test_log_evicts_oldest_and_keeps_pin(void)
{
    int i;

    probe_log_reset(&g_log);
    probe_log_add(&g_log, "init bsd rc=0");
    probe_log_pin(&g_log);
    for (i = 0; i < 2000; i++)
        probe_log_addf(&g_log, "line %d padded out to take real space", i);

    assert(g_log.truncated == 0);
    assert(g_log.evicted > 0);
    assert(g_log.len < PROBE_LOG_CAPACITY);
    assert(g_log.text[g_log.len] == '\0');
    assert(strstr(g_log.text, "init bsd rc=0") == g_log.text); /* pin held */
    assert(strstr(g_log.text, "line 1999 padded") != NULL);    /* tail held */
    assert(strstr(g_log.text, "line 0 padded") == NULL);       /* head gone */
}

static void test_config_defaults_and_parse(void)
{
    probe_config_t cfg;
    static const char ini[] =
        "# probe config\n"
        "; both comment styles\n"
        "[section]\n"
        "host = 192.168.1.10\r\n"
        "  port=8080\n"
        "net_threads = 99\n"
        "psc=1\n"
        "bgtc = 0\n"
        "bgtc_stay = 0\n"
        "bgtc_schedule = 30\n"
        "garbage line without equals\n"
        "unknown_key = 5\n"
        "run_seconds=600";

    probe_config_defaults(&cfg);
    assert(cfg.port == 80);
    assert(cfg.enable_psc == 0);  /* opt-in: psc can wedge the sleep path */
    assert(cfg.net_threads == PROBE_NET_THREADS);
    /* ScheduleTask is opt-in: leaving it on makes the window intervals
     * unreadable, which is the whole reason these three keys exist. */
    assert(cfg.bgtc_stay == 1 && cfg.bgtc_task == 1 && cfg.bgtc_schedule == 0);

    probe_config_parse(&cfg, ini, sizeof(ini) - 1);
    assert(strcmp(cfg.host, "192.168.1.10") == 0);
    assert(cfg.port == 8080);
    assert(cfg.net_threads == PROBE_NET_THREADS); /* clamped, not 99 */
    assert(cfg.enable_psc == 1);
    assert(cfg.enable_bgtc == 0);
    assert(cfg.bgtc_stay == 0);
    assert(cfg.bgtc_task == 1);       /* untouched key keeps its default */
    assert(cfg.bgtc_schedule == 30);  /* seconds, not a boolean */
    assert(cfg.run_seconds == 600);
    assert(strcmp(cfg.path, "/1GB.zip") == 0); /* untouched key keeps default */
}

int main(void)
{
    test_ring_below_capacity();
    test_ring_wrap_keeps_order_and_newest();
    test_report_holds_first_and_last();
    test_report_truncates_on_a_line_boundary();
    test_log_evicts_oldest_and_keeps_pin();
    test_config_defaults_and_parse();

    printf("probe report tests passed\n");
    return 0;
}

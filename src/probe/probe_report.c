#include "probe_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void probe_ring_reset(probe_ring_t *ring)
{
    if (!ring)
        return;
    ring->next = 0;
    ring->total = 0;
}

void probe_ring_push(probe_ring_t *ring, const probe_sample_t *sample)
{
    if (!ring || !sample)
        return;
    ring->samples[ring->next] = *sample;
    ring->next = (ring->next + 1) % PROBE_RING_CAPACITY;
    ring->total++;
}

size_t probe_ring_count(const probe_ring_t *ring)
{
    if (!ring)
        return 0;
    if (ring->total >= PROBE_RING_CAPACITY)
        return PROBE_RING_CAPACITY;
    return (size_t)ring->total;
}

const probe_sample_t *probe_ring_at(const probe_ring_t *ring, size_t index)
{
    size_t count, start;

    if (!ring)
        return NULL;
    count = probe_ring_count(ring);
    if (index >= count)
        return NULL;
    /* Once wrapped, the oldest sample sits at the write cursor. */
    start = (count == PROBE_RING_CAPACITY) ? ring->next : 0;
    return &ring->samples[(start + index) % PROBE_RING_CAPACITY];
}

void probe_log_reset(probe_log_t *log)
{
    if (!log)
        return;
    log->len = 0;
    log->pinned = 0;
    log->evicted = 0;
    log->truncated = 0;
    log->text[0] = '\0';
}

void probe_log_pin(probe_log_t *log)
{
    if (log)
        log->pinned = log->len;
}

void probe_log_add(probe_log_t *log, const char *line)
{
    size_t need;

    if (!log || !line)
        return;
    need = strlen(line);
    /* +1 newline, +1 NUL */
    while (log->len + need + 2 > PROBE_LOG_CAPACITY) {
        /* Drop the oldest unpinned line. Nothing left to drop means the line
         * cannot fit at all, and the caller loses it rather than the tail. */
        char *from = log->text + log->pinned;
        char *nl = memchr(from, '\n', log->len - log->pinned);
        size_t drop;

        if (!nl) {
            log->truncated = 1;
            return;
        }
        drop = (size_t)(nl + 1 - from);
        memmove(from, nl + 1, log->len - log->pinned - drop);
        log->len -= drop;
        log->text[log->len] = '\0';
        log->evicted++;
    }
    memcpy(log->text + log->len, line, need);
    log->len += need;
    log->text[log->len++] = '\n';
    log->text[log->len] = '\0';
}

void probe_log_addf(probe_log_t *log, const char *fmt, ...)
{
    char line[256];
    va_list ap;

    if (!log || !fmt)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    probe_log_add(log, line);
}

/* Appends when it fits whole, otherwise flags truncation and stops. */
static int append_line(char *out, size_t out_size, size_t *len, const char *line)
{
    size_t need = strlen(line);

    if (*len + need + 1 > out_size)
        return 0;
    memcpy(out + *len, line, need);
    *len += need;
    out[*len] = '\0';
    return 1;
}

size_t probe_report_format(const probe_ring_t *ring, const probe_log_t *log,
                           char *out, size_t out_size)
{
    size_t len = 0;
    size_t count, i;
    char line[512];

    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';

    if (log) {
        if (!append_line(out, out_size, &len, "# events\n"))
            return len;
        if (log->len && !append_line(out, out_size, &len, log->text))
            return len;
        if ((log->truncated || log->evicted)) {
            snprintf(line, sizeof(line), "# events truncated=%d evicted=%u\n",
                     log->truncated, log->evicted);
            if (!append_line(out, out_size, &len, line))
                return len;
        }
    }

    count = probe_ring_count(ring);
    snprintf(line, sizeof(line),
             "# samples total=%llu held=%zu\n"
             "# tick,rtc,heap_used,flags,psc_state,sock_alive,rx0,rx1,rx2,rx3,sd\n",
             (unsigned long long)(ring ? ring->total : 0), count);
    if (!append_line(out, out_size, &len, line))
        return len;

    for (i = 0; i < count; i++) {
        const probe_sample_t *s = probe_ring_at(ring, i);
        snprintf(line, sizeof(line),
                 "%llu,%llu,%llu,%u,%u,%u,%llu,%llu,%llu,%llu,%llu\n",
                 (unsigned long long)s->tick, (unsigned long long)s->rtc,
                 (unsigned long long)s->heap_used, s->flags, s->psc_state,
                 s->sock_alive, (unsigned long long)s->rx_bytes[0],
                 (unsigned long long)s->rx_bytes[1],
                 (unsigned long long)s->rx_bytes[2],
                 (unsigned long long)s->rx_bytes[3],
                 (unsigned long long)s->sd_bytes);
        if (!append_line(out, out_size, &len, line)) {
            append_line(out, out_size, &len, "# report truncated\n");
            break;
        }
    }
    return len;
}

void probe_config_defaults(probe_config_t *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    /* A plain-HTTP host with a large file; overridden from probe.ini. */
    snprintf(cfg->host, sizeof(cfg->host), "%s", "ipv4.download.thinkbroadband.com");
    snprintf(cfg->path, sizeof(cfg->path), "%s", "/1GB.zip");
    cfg->port = 80;
    cfg->report_port = 8099;
    cfg->net_threads = PROBE_NET_THREADS;
    /* psc off by default: an unacknowledged module can wedge the sleep
     * transition, so the first hardware run must opt in deliberately. */
    cfg->enable_psc = 0;
    cfg->enable_bgtc = 1;
    cfg->bgtc_scan_range = 1;
    /* Defaults reproduce the 2026-07-29 run, minus its ScheduleTask(60): that
     * one is what made the intervals unreadable, so it is opt-in now. */
    cfg->bgtc_stay = 1;
    cfg->bgtc_task = 1;
    cfg->bgtc_schedule = 0;
    cfg->enable_kept_in_sleep = 1;
    cfg->enable_kept_socket = 1;
    cfg->load_in_sleep_only = 0;
    cfg->enable_loopback_check = 1;
    cfg->enable_sd_writer = 0; /* opt-in: it writes to the card continuously */
    cfg->run_seconds = 0;
}

static void copy_value(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int parse_int(const char *src, size_t len)
{
    char buf[32];

    copy_value(buf, sizeof(buf), src, len);
    return (int)strtol(buf, NULL, 10);
}

void probe_config_parse(probe_config_t *cfg, const char *text, size_t len)
{
    size_t pos = 0;

    if (!cfg || !text)
        return;

    while (pos < len) {
        size_t line_start = pos;
        size_t line_end, key_start, key_end, eq, val_start, val_end;

        while (pos < len && text[pos] != '\n')
            pos++;
        line_end = pos;
        if (pos < len)
            pos++; /* step over the newline */
        if (line_end > line_start && text[line_end - 1] == '\r')
            line_end--;

        key_start = line_start;
        while (key_start < line_end &&
               (text[key_start] == ' ' || text[key_start] == '\t'))
            key_start++;
        if (key_start >= line_end || text[key_start] == '#' ||
            text[key_start] == ';' || text[key_start] == '[')
            continue;

        eq = key_start;
        while (eq < line_end && text[eq] != '=')
            eq++;
        if (eq >= line_end)
            continue;

        key_end = eq;
        while (key_end > key_start &&
               (text[key_end - 1] == ' ' || text[key_end - 1] == '\t'))
            key_end--;

        val_start = eq + 1;
        while (val_start < line_end &&
               (text[val_start] == ' ' || text[val_start] == '\t'))
            val_start++;
        val_end = line_end;
        while (val_end > val_start &&
               (text[val_end - 1] == ' ' || text[val_end - 1] == '\t'))
            val_end--;

        {
            size_t klen = key_end - key_start;
            size_t vlen = val_end - val_start;
            const char *k = text + key_start;
            const char *v = text + val_start;

#define KEY_IS(name) (klen == strlen(name) && memcmp(k, name, klen) == 0)
            if (KEY_IS("host"))
                copy_value(cfg->host, sizeof(cfg->host), v, vlen);
            else if (KEY_IS("path"))
                copy_value(cfg->path, sizeof(cfg->path), v, vlen);
            else if (KEY_IS("port"))
                cfg->port = (uint16_t)parse_int(v, vlen);
            else if (KEY_IS("report_port"))
                cfg->report_port = (uint16_t)parse_int(v, vlen);
            else if (KEY_IS("net_threads")) {
                int n = parse_int(v, vlen);
                if (n < 0)
                    n = 0;
                if (n > PROBE_NET_THREADS)
                    n = PROBE_NET_THREADS;
                cfg->net_threads = n;
            } else if (KEY_IS("psc"))
                cfg->enable_psc = parse_int(v, vlen) != 0;
            else if (KEY_IS("bgtc"))
                cfg->enable_bgtc = parse_int(v, vlen) != 0;
            else if (KEY_IS("bgtc_scan"))
                cfg->bgtc_scan_range = parse_int(v, vlen) != 0;
            else if (KEY_IS("bgtc_stay"))
                cfg->bgtc_stay = parse_int(v, vlen) != 0;
            else if (KEY_IS("bgtc_task"))
                cfg->bgtc_task = parse_int(v, vlen) != 0;
            else if (KEY_IS("bgtc_schedule"))
                cfg->bgtc_schedule = parse_int(v, vlen);
            else if (KEY_IS("kept_in_sleep"))
                cfg->enable_kept_in_sleep = parse_int(v, vlen) != 0;
            else if (KEY_IS("kept_socket"))
                cfg->enable_kept_socket = parse_int(v, vlen) != 0;
            else if (KEY_IS("load_in_sleep_only"))
                cfg->load_in_sleep_only = parse_int(v, vlen) != 0;
            else if (KEY_IS("loopback"))
                cfg->enable_loopback_check = parse_int(v, vlen) != 0;
            else if (KEY_IS("sd_writer"))
                cfg->enable_sd_writer = parse_int(v, vlen) != 0;
            else if (KEY_IS("run_seconds"))
                cfg->run_seconds = parse_int(v, vlen);
#undef KEY_IS
        }
    }
}

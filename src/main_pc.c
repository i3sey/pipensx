/*
 * pipensx PC entry point.
 * Usage: ./pipensx <torrent_file> [outdir]
 */
#include "core/metainfo.h"
#include "core/torrent.h"
#include "core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <curl/curl.h>

static volatile int g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <torrent_file> [outdir]\n", argv[0]);
        return 1;
    }
    const char *torrent_path = argv[1];
    const char *outdir       = argc >= 3 ? argv[2] : ".";

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    log_init("pipensx.log");

    curl_global_init(CURL_GLOBAL_DEFAULT);

    metainfo_t mi;
    if (!metainfo_load(torrent_path, &mi)) {
        fprintf(stderr, "Failed to parse '%s'\n", torrent_path);
        curl_global_cleanup();
        return 1;
    }

    torrent_t *t = torrent_create(&mi, 6881, outdir);
    if (!t) {
        fprintf(stderr, "Failed to create torrent engine\n");
        metainfo_free(&mi);
        curl_global_cleanup();
        return 1;
    }

    printf("Downloading: %s\n", mi.name);
    printf("Pieces: %u × %lld KB\n", mi.num_pieces,
           (long long)(mi.piece_length / 1024));

    uint64_t last_ui = 0;
    while (g_running) {
        int r = torrent_tick(t);

        uint64_t now = now_ms();
        if (now - last_ui >= 1000) {
            torrent_stat_t s;
            torrent_stat(t, &s);
            int pct = s.num_pieces ? (int)(s.num_pieces_done * 100 / s.num_pieces) : 0;
            char spd[16], dl[16];
            fmt_speed(spd, sizeof(spd), s.speed_bps);
            fmt_bytes(dl,  sizeof(dl),  s.downloaded);
            printf("\r[%3d%%] %s done | %s | peers=%u | dht=%u/%u    ",
                   pct, dl, spd, s.num_peers, s.dht_good, s.dht_dubious);
            fflush(stdout);
            log_msg("[status] %d%% pieces=%u/%u dl=%s spd=%s peers=%u dht=%u/%u\n",
                    pct, s.num_pieces_done, s.num_pieces,
                    dl, spd, s.num_peers, s.dht_good, s.dht_dubious);
            last_ui = now;
        }

        if (!r) {
            torrent_stat_t s;
            torrent_stat(t, &s);
            char dl[16];
            fmt_bytes(dl, sizeof(dl), s.downloaded);
            printf("\n\nDone! Downloaded %s\n", dl);
            break;
        }
    }

    torrent_destroy(t);
    metainfo_free(&mi);   /* torrent only shallow-copied mi; we own the heap members */
    curl_global_cleanup();
    return 0;
}

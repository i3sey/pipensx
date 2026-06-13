/*
 * pipensx Nintendo Switch entry point (libnx).
 * Reads first .torrent from /switch/pipensx/torrents/
 * Downloads to /switch/pipensx/downloads/
 * UI: libnx console, '+' button to exit.
 */
#include "core/metainfo.h"
#include "core/torrent.h"
#include "core/util.h"
#include <switch.h>
#include <curl/curl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TORRENT_DIR  "/switch/pipensx/torrents"
#define DOWNLOAD_DIR "/switch/pipensx/downloads"
#define DHT_NODE_DIR "/switch/pipensx"

static void render_status(const metainfo_t *mi, const torrent_stat_t *s) {
    int pct = s->complete ? 100 :
              (s->num_pieces ? (int)(s->num_pieces_done * 100 / s->num_pieces) : 0);
    char spd[16], dl[16];
    fmt_speed(spd, sizeof(spd), s->speed_bps);
    fmt_bytes(dl, sizeof(dl), s->downloaded);

    consoleClear();
    printf("pipensx " __DATE__ "\n\n");
    printf("Name     : %s\n", mi->name);
    printf("Files    : %u\n", mi->num_files);
    printf("Progress : %3d%% (%u/%u pieces)\n",
           pct, s->num_pieces_done, s->num_pieces);
    if (s->verifying)
        printf("Verifying: %u/%u pieces\n",
               s->num_pieces_verified, s->num_pieces);
    printf("Speed    : %s\n", spd);
    printf("Received : %s\n", dl);
    printf("Peers    : %u active\n", s->num_peers);
    printf("DHT nodes: %u good / %u dubious\n\n",
           s->dht_good, s->dht_dubious);
    printf("Press + to exit.\n");
    consoleUpdate(NULL);
}

static void render_message(const char *title, const char *detail) {
    consoleClear();
    printf("pipensx " __DATE__ "\n\n");
    printf("%s\n", title);
    if (detail && detail[0])
        printf("%s\n", detail);
    consoleUpdate(NULL);
}

static void make_dirs(void) {
    mkdir("/switch",                  0755);
    mkdir("/switch/pipensx",          0755);
    mkdir(TORRENT_DIR,                0755);
    mkdir(DOWNLOAD_DIR,               0755);
}

/* Find first .torrent file in dir */
static int find_torrent(const char *dir, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 8 && strcmp(ent->d_name + nlen - 8, ".torrent") == 0) {
            snprintf(out, outsz, "%s/%s", dir, ent->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int nifm_ready = 0;
    int sockets_ready = 0;
    int curl_ready = 0;

    /* Init console (must be first for printf to work) */
    consoleInit(NULL);

    printf("pipensx " __DATE__ "\n");
    printf("Initializing network...\n");
    consoleUpdate(NULL);

    /* Network init */
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        printf("nifmInitialize failed: 0x%x\n", rc);
        consoleUpdate(NULL);
        svcSleepThread(3000000000ULL);
        consoleExit(NULL);
        return 1;
    }
    nifm_ready = 1;

    if (socketInitializeDefault() != 0) {
        printf("socketInitializeDefault failed\n");
        consoleUpdate(NULL);
        svcSleepThread(3000000000ULL);
        nifmExit();
        consoleExit(NULL);
        return 1;
    }
    sockets_ready = 1;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ready = 1;
    make_dirs();
    log_init("/switch/pipensx/pipensx.log");

    /* Find torrent */
    char torrent_path[512] = "";
    printf("Looking for .torrent in %s ...\n", TORRENT_DIR);
    consoleUpdate(NULL);
    if (!find_torrent(TORRENT_DIR, torrent_path, sizeof(torrent_path))) {
        printf("No .torrent file found!\n");
        printf("Put a .torrent in %s\n", TORRENT_DIR);
        printf("Press + to exit.\n");
        consoleUpdate(NULL);
        /* Wait for + */
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        PadState pad; padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        }
        goto cleanup_exit;
    }
    printf("Found: %s\n", torrent_path);
    consoleUpdate(NULL);

    {
        metainfo_t mi;
        if (!metainfo_load(torrent_path, &mi)) {
            printf("Failed to parse torrent!\n");
            consoleUpdate(NULL);
            svcSleepThread(3000000000ULL);
            goto cleanup_exit;
        }

        torrent_t *tor = torrent_create(&mi, 51413, DOWNLOAD_DIR);
        if (!tor) {
            printf("Failed to init engine!\n");
            metainfo_free(&mi);
            consoleUpdate(NULL);
            svcSleepThread(3000000000ULL);
            goto cleanup_exit;
        }

        printf("Name : %s\n", mi.name);
        printf("Files: %u  Pieces: %u\n", mi.num_files, mi.num_pieces);
        printf("Press + to exit.\n\n");

        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        PadState pad; padInitializeDefault(&pad);

        uint64_t last_ui = 0;
        uint64_t last_log = 0;
        int user_exit = 0;
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
                user_exit = 1;
                render_message("Shutting down...", "Closing files and network.");
                break;
            }

            int r = torrent_tick(tor);

            uint64_t now = now_ms();
            if (now - last_ui >= 500) {
                torrent_stat_t s;
                torrent_stat(tor, &s);
                int pct = s.num_pieces ? (int)(s.num_pieces_done * 100 / s.num_pieces) : 0;
                char spd[16], dl[16];
                fmt_speed(spd, sizeof(spd), s.speed_bps);
                fmt_bytes(dl,  sizeof(dl),  s.downloaded);

                render_status(&mi, &s);
                last_ui = now;

                /* Write status to log file every 2 seconds */
                if (now - last_log >= 2000) {
                    log_msg("[status] %d%% pieces=%u/%u dl=%s spd=%s peers=%u dht=%u/%u\n",
                            pct, s.num_pieces_done, s.num_pieces,
                            dl, spd, s.num_peers, s.dht_good, s.dht_dubious);
                    last_log = now;
                }
            }

            if (!r) {
                log_msg("[torrent] download complete: files saved to %s\n", DOWNLOAD_DIR);
                torrent_stat_t s;
                torrent_stat(tor, &s);
                render_status(&mi, &s);
                printf("\nDownload complete and verified.\n");
                printf("Files saved to: %s\n", DOWNLOAD_DIR);
                printf("Press + to exit.\n");
                consoleUpdate(NULL);
                /* Stay open until + pressed */
                while (appletMainLoop()) {
                    padUpdate(&pad);
                    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
                        user_exit = 1;
                        render_message("Shutting down...", "Closing files and network.");
                        break;
                    }
                }
                break;
            }
        }

        torrent_destroy(tor);
        metainfo_free(&mi);
        if (user_exit)
            svcSleepThread(200000000ULL);
    }

cleanup_exit:
    log_close();
    if (curl_ready) curl_global_cleanup();
    if (sockets_ready) socketExit();
    if (nifm_ready) nifmExit();
    consoleExit(NULL);
    return 0;
}

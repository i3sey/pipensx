#include "app/update_transaction.h"

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *DefaultTarget = "sdmc:/switch/pipensx/pipensx.nro";
static const char *LogPath = "sdmc:/switch/pipensx/pipensx-update.log";

static void update_log(const char *format, ...) {
    FILE *file = fopen(LogPath, "ab");
    if (!file)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fflush(file);
    fclose(file);
}

int main(int argc, char **argv) {
    const bool requested = argc > 1 && argv[1] &&
                           strcmp(argv[1], "--finish-update") == 0;
    update_log("[helper] started requested=%d argc=%d nextload=%d\n",
               requested ? 1 : 0, argc, envHasNextLoad() ? 1 : 0);
    /* Never re-launch pipensx. Relaunching the full app inside the same
     * hbloader session re-initializes the graphics/applet/service stack a
     * second time and crashes (black screen then fatal). Every path below
     * returns with no nextload set, so the loader drops to HOME and the user
     * relaunches a fresh process manually. */
    if (!requested)
        return 0;

    const char *target = DefaultTarget;
    if (argc > 2 && argv[2] && argv[2][0] != '\0') {
        target = argv[2];
    }
    update_log("[helper] target=%s\n", target);

    char target_buf[FS_MAX_PATH];
    char staged_buf[FS_MAX_PATH];
    char marker_buf[FS_MAX_PATH];
    char backup_buf[FS_MAX_PATH];

    snprintf(target_buf, sizeof(target_buf), "%s", target);
    snprintf(staged_buf, sizeof(staged_buf), "%s.update", target);
    snprintf(marker_buf, sizeof(marker_buf), "%s.update.sha256", target);
    snprintf(backup_buf, sizeof(backup_buf), "%s.previous", target);

    update_paths_t paths = {target_buf, staged_buf, marker_buf, backup_buf};
    char error[256] = {0};

    if (!update_transaction_apply(&paths, error, sizeof(error))) {
        update_log("[helper] apply failed: %s\n", error);
        return 0;
    }
    Result commit = fsdevCommitDevice("sdmc");
    if (R_FAILED(commit)) {
        update_log("[helper] commit failed result=0x%08x\n", commit);
        error[0] = '\0';
        if (!update_transaction_rollback(&paths, error, sizeof(error)))
            update_log("[helper] rollback failed: %s\n", error);
        fsdevCommitDevice("sdmc");
        return 0;
    }
    update_log("[helper] swap committed; returning to HOME\n");

    /* If an alternate known installation path exists on the SD card
     * (e.g. sdmc:/switch/pipensx.nro vs sdmc:/switch/pipensx/pipensx.nro),
     * keep it in sync with the newly installed binary so forwarders and
     * hbmenu entries targeting either path always launch the updated version. */
    const char *alt_target = NULL;
    if (strcmp(target_buf, "sdmc:/switch/pipensx/pipensx.nro") == 0) {
        alt_target = "sdmc:/switch/pipensx.nro";
    } else if (strcmp(target_buf, "sdmc:/switch/pipensx.nro") == 0) {
        alt_target = "sdmc:/switch/pipensx/pipensx.nro";
    }

    if (alt_target && access(alt_target, F_OK) == 0) {
        update_log("[helper] syncing to alternate path %s\n", alt_target);
        char alt_tmp[FS_MAX_PATH];
        snprintf(alt_tmp, sizeof(alt_tmp), "%s.tmp", alt_target);
        unlink(alt_tmp);
        FILE *src = fopen(target_buf, "rb");
        FILE *dst = fopen(alt_tmp, "wb");
        if (src && dst) {
            char buf[64 * 1024];
            size_t n;
            bool ok = true;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                if (fwrite(buf, 1, n, dst) != n) {
                    ok = false;
                    break;
                }
            }
            fclose(src);
            fclose(dst);
            if (ok) {
                unlink(alt_target);
                if (rename(alt_tmp, alt_target) == 0) {
                    fsdevCommitDevice("sdmc");
                    update_log("[helper] alternate path updated\n");
                } else {
                    unlink(alt_tmp);
                }
            } else {
                unlink(alt_tmp);
            }
        } else {
            if (src) fclose(src);
            if (dst) fclose(dst);
            unlink(alt_tmp);
        }
    }

    return 0;
}

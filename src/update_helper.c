#include "app/update_transaction.h"

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *Target = "sdmc:/switch/pipensx/pipensx.nro";
static const char *Staged = "sdmc:/switch/pipensx/pipensx.nro.update";
static const char *Marker = "sdmc:/switch/pipensx/pipensx.nro.update.sha256";
static const char *Backup = "sdmc:/switch/pipensx/pipensx.nro.previous";
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
    update_paths_t paths = {Target, Staged, Marker, Backup};
    char error[256] = {0};
    const bool requested = argc > 1 && argv[1] &&
                           strcmp(argv[1], "--finish-update") == 0;
    update_log("[helper] started requested=%d nextload=%d\n",
               requested ? 1 : 0, envHasNextLoad() ? 1 : 0);
    /* Never re-launch pipensx. Relaunching the full app inside the same
     * hbloader session re-initializes the graphics/applet/service stack a
     * second time and crashes (black screen then fatal). Every path below
     * returns with no nextload set, so the loader drops to HOME and the user
     * relaunches a fresh process manually. */
    if (!requested)
        return 0;
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
    return 0;
}

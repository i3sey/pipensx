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

static void return_to_target(void) {
    char arguments[512];
    snprintf(arguments, sizeof(arguments), "\"%s\"", Target);
    Result result = envSetNextLoad(Target, arguments);
    update_log("[helper] nextload target result=0x%08x\n", result);
}

int main(int argc, char **argv) {
    update_paths_t paths = {Target, Staged, Marker, Backup};
    char error[256] = {0};
    const bool requested = argc > 1 && argv[1] &&
                           strcmp(argv[1], "--finish-update") == 0;
    update_log("[helper] started requested=%d nextload=%d\n",
               requested ? 1 : 0, envHasNextLoad() ? 1 : 0);
    if (!requested || !envHasNextLoad()) {
        return_to_target();
        return 0;
    }
    if (!update_transaction_apply(&paths, error, sizeof(error))) {
        update_log("[helper] apply failed: %s\n", error);
        return_to_target();
        return 0;
    }
    Result commit = fsdevCommitDevice("sdmc");
    if (R_FAILED(commit)) {
        update_log("[helper] commit failed result=0x%08x\n", commit);
        error[0] = '\0';
        if (!update_transaction_rollback(&paths, error, sizeof(error)))
            update_log("[helper] rollback failed: %s\n", error);
        fsdevCommitDevice("sdmc");
        return_to_target();
        return 0;
    }
    char arguments[512];
    snprintf(arguments, sizeof(arguments), "\"%s\"", Target);
    Result next = envSetNextLoad(Target, arguments);
    if (R_FAILED(next)) {
        update_log("[helper] nextload failed result=0x%08x\n", next);
        error[0] = '\0';
        if (!update_transaction_rollback(&paths, error, sizeof(error)))
            update_log("[helper] rollback failed: %s\n", error);
        fsdevCommitDevice("sdmc");
        return_to_target();
        return 0;
    }
    update_log("[helper] swap committed; launching target\n");
    return 0;
}

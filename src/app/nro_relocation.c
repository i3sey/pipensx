#include "nro_relocation.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *error, size_t size, const char *message) {
    if (error && size)
        snprintf(error, size, "%s", message ? message : "NRO relocation failed");
}

static void set_errno_error(char *error, size_t size, const char *operation) {
    if (error && size)
        snprintf(error, size, "%s: %s", operation, strerror(errno));
}

static bool has_nro_extension(const char *name) {
    const size_t length = strlen(name);
    return length > 4 && name[length - 4] == '.' &&
           tolower((unsigned char)name[length - 3]) == 'n' &&
           tolower((unsigned char)name[length - 2]) == 'r' &&
           tolower((unsigned char)name[length - 1]) == 'o';
}

bool nro_relocation_source(const char *launch_path, char *source,
                           size_t source_size) {
    static const char SdmcPrefix[] = "sdmc:/switch/";
    static const char RootPrefix[] = "/switch/";
    if (!launch_path || !source || source_size == 0)
        return false;

    const char *name = NULL;
    if (strncmp(launch_path, SdmcPrefix, sizeof(SdmcPrefix) - 1) == 0) {
        name = launch_path + sizeof(SdmcPrefix) - 1;
        if (strchr(name, '/') || !has_nro_extension(name))
            return false;
        if (snprintf(source, source_size, "%s", launch_path) >=
            (int)source_size)
            return false;
        return true;
    }
    if (strncmp(launch_path, RootPrefix, sizeof(RootPrefix) - 1) != 0)
        return false;
    name = launch_path + sizeof(RootPrefix) - 1;
    if (strchr(name, '/') || !has_nro_extension(name))
        return false;
    return snprintf(source, source_size, "sdmc:%s", launch_path) <
           (int)source_size;
}

bool nro_relocation_apply(const nro_relocation_paths_t *paths,
                          char *error, size_t error_size) {
    if (!paths || !paths->source || !paths->target || !paths->backup) {
        set_error(error, error_size, "NRO relocation paths are missing");
        return false;
    }
    struct stat info;
    if (stat(paths->source, &info) != 0) {
        set_errno_error(error, error_size, "Unable to inspect launched NRO");
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        errno = EINVAL;
        set_errno_error(error, error_size, "Unable to inspect launched NRO");
        return false;
    }
    if (unlink(paths->backup) != 0 && errno != ENOENT) {
        set_errno_error(error, error_size, "Unable to remove stale relocation backup");
        return false;
    }

    const bool had_target = access(paths->target, F_OK) == 0;
    if (had_target && rename(paths->target, paths->backup) != 0) {
        set_errno_error(error, error_size, "Unable to back up installed NRO");
        return false;
    }
    if (rename(paths->source, paths->target) == 0)
        return true;

    const int saved_errno = errno;
    if (had_target)
        rename(paths->backup, paths->target);
    errno = saved_errno;
    set_errno_error(error, error_size, "Unable to move launched NRO");
    return false;
}

bool nro_relocation_rollback(const nro_relocation_paths_t *paths,
                             char *error, size_t error_size) {
    if (!paths || !paths->source || !paths->target || !paths->backup) {
        set_error(error, error_size, "NRO relocation paths are missing");
        return false;
    }
    if (rename(paths->target, paths->source) != 0) {
        set_errno_error(error, error_size, "Unable to restore launched NRO");
        return false;
    }
    if (access(paths->backup, F_OK) != 0)
        return true;
    if (rename(paths->backup, paths->target) == 0)
        return true;

    const int saved_errno = errno;
    /* Keep a launchable NRO at the canonical path if restoring the old one
     * fails. The new NRO is preferable to leaving the app unlaunchable. */
    rename(paths->source, paths->target);
    errno = saved_errno;
    set_errno_error(error, error_size, "Unable to restore installed NRO");
    return false;
}

bool nro_relocation_confirm(const nro_relocation_paths_t *paths,
                            char *error, size_t error_size) {
    if (!paths || !paths->backup) {
        set_error(error, error_size, "NRO relocation paths are missing");
        return false;
    }
    if (unlink(paths->backup) != 0 && errno != ENOENT) {
        set_errno_error(error, error_size, "Unable to remove relocation backup");
        return false;
    }
    return true;
}

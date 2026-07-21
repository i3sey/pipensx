#include "update_transaction.h"

#include "../core/sha256.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UPDATE_NRO_LIMIT (64u * 1024u * 1024u)

static void set_error(char *error, size_t size, const char *message) {
    if (error && size)
        snprintf(error, size, "%s", message ? message : "update failed");
}

static void set_errno_error(char *error, size_t size, const char *operation) {
    if (error && size)
        snprintf(error, size, "%s: %s", operation, strerror(errno));
}

static bool read_expected(const char *path, char expected[65],
                          char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        set_errno_error(error, error_size, "Unable to open checksum marker");
        return false;
    }
    char token[66] = {0};
    const int matched = fscanf(file, "%65s", token);
    fclose(file);
    if (matched != 1 || strlen(token) != 64) {
        set_error(error, error_size, "Staged update checksum is invalid");
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)token[i])) {
            set_error(error, error_size, "Staged update checksum is invalid");
            return false;
        }
        expected[i] = (char)tolower((unsigned char)token[i]);
    }
    expected[64] = '\0';
    return true;
}

static bool checksum_file(const char *path, char actual[65],
                          char *error, size_t error_size) {
    struct stat info;
    if (stat(path, &info) != 0) {
        set_errno_error(error, error_size, "Unable to stat staged update");
        return false;
    }
    if (info.st_size <= 0 || (uint64_t)info.st_size > UPDATE_NRO_LIMIT) {
        set_error(error, error_size, "Staged update size is invalid");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        set_errno_error(error, error_size, "Unable to open staged update");
        return false;
    }
    const size_t size = (size_t)info.st_size;
    uint8_t *data = malloc(size);
    if (!data) {
        fclose(file);
        set_error(error, error_size, "Unable to allocate checksum buffer");
        return false;
    }
    const bool read_ok = fread(data, 1, size, file) == size;
    fclose(file);
    if (!read_ok) {
        free(data);
        set_error(error, error_size, "Unable to read staged update");
        return false;
    }
    uint8_t digest[32];
    static const char digits[] = "0123456789abcdef";
    sha256(data, size, digest);
    free(data);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        actual[i * 2] = digits[digest[i] >> 4];
        actual[i * 2 + 1] = digits[digest[i] & 15];
    }
    actual[64] = '\0';
    return true;
}

bool update_transaction_ready(const update_paths_t *paths,
                              char *error, size_t error_size) {
    char expected[65];
    char actual[65];
    if (!paths || !read_expected(paths->marker, expected, error, error_size) ||
        !checksum_file(paths->staged, actual, error, error_size))
        return false;
    if (strcmp(expected, actual) != 0) {
        set_error(error, error_size, "Staged update checksum does not match");
        return false;
    }
    return true;
}

bool update_transaction_apply(const update_paths_t *paths,
                              char *error, size_t error_size) {
    if (!update_transaction_ready(paths, error, error_size))
        return false;
    unlink(paths->backup);
    const bool had_target = access(paths->target, F_OK) == 0;
    if (had_target && rename(paths->target, paths->backup) != 0) {
        set_errno_error(error, error_size, "Unable to back up current NRO");
        return false;
    }
    if (rename(paths->staged, paths->target) != 0) {
        const int saved_errno = errno;
        if (had_target)
            rename(paths->backup, paths->target);
        errno = saved_errno;
        set_errno_error(error, error_size, "Unable to install staged NRO");
        return false;
    }
    char expected[65];
    char actual[65];
    if (!read_expected(paths->marker, expected, error, error_size) ||
        !checksum_file(paths->target, actual, error, error_size) ||
        strcmp(expected, actual) != 0) {
        update_transaction_rollback(paths, NULL, 0);
        if (error && error_size && error[0] == '\0')
            set_error(error, error_size, "Installed NRO checksum does not match");
        return false;
    }
    return true;
}

bool update_transaction_rollback(const update_paths_t *paths,
                                 char *error, size_t error_size) {
    if (!paths || access(paths->backup, F_OK) != 0) {
        set_error(error, error_size, "Update backup is missing");
        return false;
    }
    unlink(paths->staged);
    if (access(paths->target, F_OK) == 0 &&
        rename(paths->target, paths->staged) != 0) {
        set_errno_error(error, error_size, "Unable to restage failed NRO");
        return false;
    }
    if (rename(paths->backup, paths->target) != 0) {
        const int saved_errno = errno;
        /* Keep at least one launchable NRO at the canonical path even when
         * restoring the backup fails (for example after an SD I/O error). */
        rename(paths->staged, paths->target);
        errno = saved_errno;
        set_errno_error(error, error_size, "Unable to restore previous NRO");
        return false;
    }
    return true;
}

bool update_transaction_confirm(const update_paths_t *paths,
                                char *error, size_t error_size) {
    if (!paths) {
        set_error(error, error_size, "Update paths are missing");
        return false;
    }
    if (unlink(paths->backup) != 0 && errno != ENOENT) {
        set_errno_error(error, error_size, "Unable to remove update backup");
        return false;
    }
    if (unlink(paths->marker) != 0 && errno != ENOENT) {
        set_errno_error(error, error_size, "Unable to remove checksum marker");
        return false;
    }
    return true;
}

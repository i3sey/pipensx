#include "app/update_transaction.h"
#include "core/sha256.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *Root = "/tmp/pipensx-update-transaction";
static const char *FailBackup = NULL;
static const char *FailTarget = NULL;

int __real_rename(const char *old_path, const char *new_path);

int __wrap_rename(const char *old_path, const char *new_path) {
    if (FailBackup && FailTarget && strcmp(old_path, FailBackup) == 0 &&
        strcmp(new_path, FailTarget) == 0) {
        errno = EIO;
        return -1;
    }
    return __real_rename(old_path, new_path);
}

static void write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    rewind(file);
    char *data = calloc((size_t)size + 1, 1);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    return data;
}

static void write_marker(const char *path, const void *data, size_t size) {
    uint8_t digest[32];
    static const char digits[] = "0123456789abcdef";
    char text[66];
    sha256(data, size, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        text[i * 2] = digits[digest[i] >> 4];
        text[i * 2 + 1] = digits[digest[i] & 15];
    }
    text[64] = '\n';
    text[65] = '\0';
    write_file(path, text, 65);
}

static void cleanup(const update_paths_t *paths) {
    unlink(paths->target);
    unlink(paths->staged);
    unlink(paths->marker);
    unlink(paths->backup);
}

static void test_apply_and_rollback(void) {
    char target[256], staged[256], marker[256], backup[256];
    snprintf(target, sizeof(target), "%s.nro", Root);
    snprintf(staged, sizeof(staged), "%s.nro.update", Root);
    snprintf(marker, sizeof(marker), "%s.nro.update.sha256", Root);
    snprintf(backup, sizeof(backup), "%s.nro.previous", Root);
    update_paths_t paths = {target, staged, marker, backup};
    cleanup(&paths);

    const char old_data[] = "old-version";
    const char new_data[] = "new-version";
    write_file(target, old_data, sizeof(old_data) - 1);
    write_file(staged, new_data, sizeof(new_data) - 1);
    write_marker(marker, new_data, sizeof(new_data) - 1);

    char error[256] = {0};
    assert(update_transaction_apply(&paths, error, sizeof(error)));
    char *installed = read_file(target);
    char *saved = read_file(backup);
    assert(strcmp(installed, new_data) == 0);
    assert(strcmp(saved, old_data) == 0);
    assert(access(staged, F_OK) != 0);
    free(installed);
    free(saved);

    assert(update_transaction_rollback(&paths, error, sizeof(error)));
    char *restored = read_file(target);
    char *restaged = read_file(staged);
    assert(strcmp(restored, old_data) == 0);
    assert(strcmp(restaged, new_data) == 0);
    free(restored);
    free(restaged);

    assert(update_transaction_apply(&paths, error, sizeof(error)));
    assert(update_transaction_confirm(&paths, error, sizeof(error)));
    installed = read_file(target);
    assert(strcmp(installed, new_data) == 0);
    assert(access(backup, F_OK) != 0);
    assert(access(marker, F_OK) != 0);
    free(installed);
    cleanup(&paths);
}

static void test_corrupt_stage_is_rejected(void) {
    char target[256], staged[256], marker[256], backup[256];
    snprintf(target, sizeof(target), "%s.nro", Root);
    snprintf(staged, sizeof(staged), "%s.nro.update", Root);
    snprintf(marker, sizeof(marker), "%s.nro.update.sha256", Root);
    snprintf(backup, sizeof(backup), "%s.nro.previous", Root);
    update_paths_t paths = {target, staged, marker, backup};
    cleanup(&paths);
    write_file(target, "old", 3);
    write_file(staged, "expected", 8);
    write_marker(marker, "expected", 8);
    write_file(staged, "corrupt", 7);
    char error[256] = {0};
    assert(!update_transaction_apply(&paths, error, sizeof(error)));
    char *preserved = read_file(target);
    assert(strcmp(preserved, "old") == 0);
    assert(access(backup, F_OK) != 0);
    free(preserved);
    cleanup(&paths);
}

static void test_rollback_restore_failure_keeps_canonical_target(void) {
    char target[256], staged[256], marker[256], backup[256];
    snprintf(target, sizeof(target), "%s.nro", Root);
    snprintf(staged, sizeof(staged), "%s.nro.update", Root);
    snprintf(marker, sizeof(marker), "%s.nro.update.sha256", Root);
    snprintf(backup, sizeof(backup), "%s.nro.previous", Root);
    update_paths_t paths = {target, staged, marker, backup};
    cleanup(&paths);
    write_file(target, "new-version", 11);
    write_file(backup, "old-version", 11);

    FailBackup = backup;
    FailTarget = target;
    char error[256] = {0};
    assert(!update_transaction_rollback(&paths, error, sizeof(error)));
    FailBackup = NULL;
    FailTarget = NULL;

    char *launchable = read_file(target);
    char *preserved_backup = read_file(backup);
    assert(strcmp(launchable, "new-version") == 0);
    assert(strcmp(preserved_backup, "old-version") == 0);
    assert(access(staged, F_OK) != 0);
    free(launchable);
    free(preserved_backup);
    cleanup(&paths);
}

int main(void) {
    test_apply_and_rollback();
    test_corrupt_stage_is_rejected();
    test_rollback_restore_failure_keeps_canonical_target();
    puts("update transaction tests passed");
    return 0;
}

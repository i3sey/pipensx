#include "app/nro_relocation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *Root = "/tmp/pipensx-nro-relocation";

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
    assert(fclose(file) == 0);
}

static void assert_contents(const char *path, const char *expected) {
    char contents[64] = {0};
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(contents, 1, sizeof(contents) - 1, file) == strlen(expected));
    assert(fclose(file) == 0);
    assert(strcmp(contents, expected) == 0);
}

static void cleanup(const nro_relocation_paths_t *paths) {
    unlink(paths->source);
    unlink(paths->target);
    unlink(paths->backup);
    rmdir(Root);
}

static void test_source_detection(void) {
    char source[128];
    assert(nro_relocation_source("sdmc:/switch/pipensx.nro", source,
                                 sizeof(source)));
    assert(strcmp(source, "sdmc:/switch/pipensx.nro") == 0);
    assert(nro_relocation_source("/switch/PipeNSX.NRO", source,
                                 sizeof(source)));
    assert(strcmp(source, "sdmc:/switch/PipeNSX.NRO") == 0);
    assert(!nro_relocation_source(
        "sdmc:/switch/pipensx/pipensx.nro", source, sizeof(source)));
    assert(!nro_relocation_source("sdmc:/apps/pipensx.nro", source,
                                  sizeof(source)));
    assert(!nro_relocation_source("sdmc:/switch/not-an-nro", source,
                                  sizeof(source)));
}

static void test_replace_and_confirm(void) {
    char source[128], target[128], backup[128];
    snprintf(source, sizeof(source), "%s/source.nro", Root);
    snprintf(target, sizeof(target), "%s/pipensx.nro", Root);
    snprintf(backup, sizeof(backup), "%s/relocation-backup", Root);
    const nro_relocation_paths_t paths = {source, target, backup};
    cleanup(&paths);
    assert(mkdir(Root, 0700) == 0);
    write_file(source, "new");
    write_file(target, "old");

    char error[256] = {0};
    assert(nro_relocation_apply(&paths, error, sizeof(error)));
    assert(access(source, F_OK) != 0);
    assert_contents(target, "new");
    assert_contents(backup, "old");
    assert(nro_relocation_confirm(&paths, error, sizeof(error)));
    assert(access(backup, F_OK) != 0);
    cleanup(&paths);
}

static void test_rollback(void) {
    char source[128], target[128], backup[128];
    snprintf(source, sizeof(source), "%s/source.nro", Root);
    snprintf(target, sizeof(target), "%s/pipensx.nro", Root);
    snprintf(backup, sizeof(backup), "%s/relocation-backup", Root);
    const nro_relocation_paths_t paths = {source, target, backup};
    cleanup(&paths);
    assert(mkdir(Root, 0700) == 0);
    write_file(source, "new");
    write_file(target, "old");

    char error[256] = {0};
    assert(nro_relocation_apply(&paths, error, sizeof(error)));
    assert(nro_relocation_rollback(&paths, error, sizeof(error)));
    assert_contents(source, "new");
    assert_contents(target, "old");
    assert(access(backup, F_OK) != 0);
    cleanup(&paths);
}

int main(void) {
    test_source_detection();
    test_replace_and_confirm();
    test_rollback();
    puts("nro relocation tests passed");
    return 0;
}

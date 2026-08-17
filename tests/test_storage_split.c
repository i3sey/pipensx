/* Split-folder storage (DBI archive format): files above the FAT32 ceiling
   are stored as a folder of preallocated parts when the >4 GiB probe fails.
   force_split drives the split path deterministically on PC. */
#include "../src/core/metainfo.h"
#include "../src/platform/storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PART ((int64_t)0xFFFF0000u) /* DBI split part size */

static void init_single_file_metainfo(metainfo_t *mi, const char *name,
                                      int64_t length) {
    memset(mi, 0, sizeof(*mi));
    snprintf(mi->name, sizeof(mi->name), "%s", name);
    mi->piece_length = 1 << 20;
    mi->total_length = length;
    mi->num_pieces = (uint32_t)((length + mi->piece_length - 1) /
                                mi->piece_length);
    mi->num_files = 1;
    mi->files = (mi_file_t*)calloc(1, sizeof(*mi->files));
    assert(mi->files);
    snprintf(mi->files[0].path, sizeof(mi->files[0].path), "%s", name);
    mi->files[0].length = length;
}

static void fill_pattern(uint8_t *data, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++)
        data[i] = (uint8_t)(seed + i * 31u);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void path_join(char *out, size_t out_size, const char *dir,
                      const char *name) {
    int len = snprintf(out, out_size, "%s/%s", dir, name);
    assert(len >= 0 && (size_t)len < out_size);
}

static int file_size(const char *path, int64_t *size) {
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
        return 0;
    *size = (int64_t)st.st_size;
    return 1;
}

/* Write a pattern region and return its verification pair. */
typedef struct { int64_t off; size_t len; uint8_t seed; } region_t;

static void write_region(storage_t *s, const region_t *r) {
    uint8_t *buf = (uint8_t*)malloc(r->len);
    assert(buf);
    fill_pattern(buf, r->len, r->seed);
    assert(storage_write(s, r->off, buf, r->len));
    free(buf);
}

static void check_region(storage_t *s, const region_t *r) {
    uint8_t *want = (uint8_t*)malloc(r->len);
    uint8_t *got = (uint8_t*)malloc(r->len);
    assert(want && got);
    fill_pattern(want, r->len, r->seed);
    assert(storage_read(s, r->off, got, r->len) == (int)r->len);
    assert(memcmp(want, got, r->len) == 0);
    free(want);
    free(got);
}

static void test_split_write_read_resume(void) {
    char outdir[] = "/tmp/pipensx-split-XXXXXX";
    assert(mkdtemp(outdir));

    const int64_t length = 2 * PART + PART / 2; /* 2.5 parts */
    metainfo_t mi;
    init_single_file_metainfo(&mi, "big.bin", length);

    storage_file_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = STORAGE_FILE_DISK;
    cfg.force_split = 1;

    storage_t *s = storage_open_ex(&mi, outdir, &cfg);
    assert(s);
    assert(storage_error(s)[0] == 0);

    char dir[512];
    char part_path[512];
    snprintf(dir, sizeof(dir), "%s/big.bin", outdir);
    assert(is_dir(dir));
    int64_t sz;
    path_join(part_path, sizeof(part_path), dir, "00");
    assert(file_size(part_path, &sz) && sz == PART);
    path_join(part_path, sizeof(part_path), dir, "01");
    assert(file_size(part_path, &sz) && sz == PART);
    path_join(part_path, sizeof(part_path), dir, "02");
    assert(file_size(part_path, &sz) && sz == PART / 2);

    /* Regions: file head, both part boundaries, mid-part, file tail. */
    const region_t regions[] = {
        { 0, 4096, 1 },
        { PART - 100, 200, 2 },          /* crosses part 0 -> 1 */
        { 2 * PART - 100, 200, 3 },      /* crosses part 1 -> 2 */
        { PART + PART / 2 - 64, 128, 4 },/* middle of part 1 */
        { length - 4096, 4096, 5 },
    };
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        write_region(s, &regions[i]);
    assert(storage_flush(s));

    /* Untouched sparse area reads back as zeros. */
    uint8_t zeros[64], buf[64];
    memset(buf, 0xAB, sizeof(buf));
    assert(storage_read(s, PART / 2 - 32, buf, sizeof(buf)) ==
           (int)sizeof(buf));
    memset(zeros, 0, sizeof(zeros));
    assert(memcmp(buf, zeros, sizeof(buf)) == 0);

    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        check_region(s, &regions[i]);

    /* The split folder is found by the same locator as ordinary files. */
    char located[512];
    assert(storage_locate_file_path(&mi, outdir, 0, located,
                                    sizeof(located)));
    assert(strcmp(located, dir) == 0);

    storage_close(s);

    /* Resume: reopen the folder, existing parts are read directly. */
    s = storage_open_ex(&mi, outdir, &cfg);
    assert(s);
    assert(is_dir(dir));
    check_region(s, &regions[1]); /* boundary read through two parts */
    check_region(s, &regions[4]); /* tail */

    const region_t resume_region = { PART + 12345, 256, 6 };
    write_region(s, &resume_region);
    check_region(s, &resume_region);
    assert(storage_flush(s));
    storage_close(s);

    /* Parts keep their sizes across sessions. */
    path_join(part_path, sizeof(part_path), dir, "00");
    assert(file_size(part_path, &sz) && sz == PART);
    path_join(part_path, sizeof(part_path), dir, "02");
    assert(file_size(part_path, &sz) && sz == PART / 2);

    path_join(part_path, sizeof(part_path), dir, "00");
    unlink(part_path);
    path_join(part_path, sizeof(part_path), dir, "01");
    unlink(part_path);
    path_join(part_path, sizeof(part_path), dir, "02");
    unlink(part_path);
    rmdir(dir);
    rmdir(outdir);
    free(mi.files);
}

static void test_small_file_stays_plain(void) {
    char outdir[] = "/tmp/pipensx-small-XXXXXX";
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "small.bin", 10 * 1024 * 1024);

    storage_file_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = STORAGE_FILE_DISK;
    cfg.force_split = 1; /* must be ignored below the FAT32 ceiling */

    storage_t *s = storage_open_ex(&mi, outdir, &cfg);
    assert(s);
    char path[512];
    snprintf(path, sizeof(path), "%s/small.bin", outdir);
    assert(!is_dir(path));
    int64_t sz;
    assert(file_size(path, &sz) && sz == 10 * 1024 * 1024);

    const region_t r = { 4096, 1024, 9 };
    write_region(s, &r);
    assert(storage_flush(s));
    check_region(s, &r);
    storage_close(s);

    unlink(path);
    rmdir(outdir);
    free(mi.files);
}

static void test_large_file_stays_plain_when_probe_passes(void) {
    char outdir[] = "/tmp/pipensx-plain-XXXXXX";
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "huge.bin", (int64_t)5 * 1024 * 1024 * 1024);

    storage_file_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = STORAGE_FILE_DISK;

    storage_t *s = storage_open_ex(&mi, outdir, &cfg);
    assert(s);
    char path[512];
    snprintf(path, sizeof(path), "%s/huge.bin", outdir);
    assert(!is_dir(path));

    const region_t r = { 0, 512, 3 };
    write_region(s, &r);
    assert(storage_flush(s));
    check_region(s, &r);
    storage_close(s);

    unlink(path);
    rmdir(outdir);
    free(mi.files);
}

int main(void) {
    test_split_write_read_resume();
    test_small_file_stays_plain();
    test_large_file_stays_plain_when_probe_passes();
    puts("storage split tests passed");
    return 0;
}

#include "../src/core/piece.h"
#include "../src/core/sha1.h"
#include "../src/core/util.h"
#include "../src/platform/storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void init_single_file_metainfo(metainfo_t *mi, const char *name,
                                      int64_t piece_length, int64_t total_length) {
    memset(mi, 0, sizeof(*mi));
    snprintf(mi->name, sizeof(mi->name), "%s", name);
    mi->piece_length = piece_length;
    mi->total_length = total_length;
    mi->num_pieces = (uint32_t)((total_length + piece_length - 1) / piece_length);
    mi->piece_hashes = (uint8_t*)calloc(mi->num_pieces, 20);
    mi->num_files = 1;
    mi->files = (mi_file_t*)calloc(1, sizeof(*mi->files));
    assert(mi->piece_hashes);
    assert(mi->files);
    snprintf(mi->files[0].path, sizeof(mi->files[0].path), "%s", name);
    mi->files[0].length = total_length;
}

static void free_test_metainfo(metainfo_t *mi) {
    free(mi->piece_hashes);
    free(mi->files);
}

static void fill_pattern(uint8_t *data, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++)
        data[i] = (uint8_t)(seed + i * 31u);
}

static void cleanup_output(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
    rmdir(dir);
}

static void test_large_piece_and_short_last_piece(void) {
    const int64_t piece_length = 1024 * 1024;
    const size_t tail_length = 12345;
    const size_t total_length = (size_t)piece_length + tail_length;
    char outdir[] = "/tmp/pipensx-piece-large-XXXXXX";
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "large.bin", piece_length, total_length);

    uint8_t *expected = (uint8_t*)malloc(total_length);
    uint8_t *actual = (uint8_t*)malloc(total_length);
    assert(expected);
    assert(actual);
    fill_pattern(expected, total_length, 7);
    sha1(expected, (size_t)piece_length, mi.piece_hashes);
    sha1(expected + piece_length, tail_length, mi.piece_hashes + 20);

    storage_t *store = storage_open(&mi, outdir);
    assert(store);
    piece_mgr_t *pm = piece_mgr_create(&mi, store);
    assert(pm);
    assert(pm->slots[0].num_blocks == 64);

    piece_mgr_mark_block_requested(pm, 0, 0);
    assert(piece_mgr_block_requested(pm, 0, 0));
    for (uint32_t block = 0; block < 32; block++) {
        int result = piece_mgr_got_block(pm, 0, block * BLOCK_SIZE,
                                         expected + block * BLOCK_SIZE, BLOCK_SIZE);
        assert(result == 1);
    }
    assert(!piece_mgr_block_requested(pm, 0, 0));
    assert(pm->num_done == 0);
    assert(pm->slots[0].state == PS_PENDING);

    for (uint32_t block = 32; block < 64; block++) {
        int result = piece_mgr_got_block(pm, 0, block * BLOCK_SIZE,
                                         expected + block * BLOCK_SIZE, BLOCK_SIZE);
        assert(result == (block == 63 ? 2 : 1));
    }
    assert(pm->num_done == 1);
    assert(pm->completed_bytes == (uint64_t)piece_length);

    assert(piece_mgr_got_block(pm, 1, 0, expected + piece_length,
                               (uint32_t)tail_length) == 2);
    assert(pm->num_done == 2);
    assert(pm->completed_bytes == (uint64_t)total_length);
    assert(piece_mgr_verify_all(pm));
    assert(storage_read(store, 0, actual, total_length) == (int)total_length);
    assert(memcmp(actual, expected, total_length) == 0);

    piece_mgr_destroy(pm);
    storage_close(store);
    free(actual);
    free(expected);
    free_test_metainfo(&mi);
    cleanup_output(outdir, "large.bin");
}

static void test_hash_mismatch_resets_all_blocks(void) {
    const size_t piece_length = 4 * BLOCK_SIZE;
    char outdir[] = "/tmp/pipensx-piece-reset-XXXXXX";
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "retry.bin", piece_length, piece_length);

    uint8_t *expected = (uint8_t*)malloc(piece_length);
    uint8_t *corrupt = (uint8_t*)malloc(piece_length);
    assert(expected);
    assert(corrupt);
    fill_pattern(expected, piece_length, 19);
    memcpy(corrupt, expected, piece_length);
    corrupt[piece_length - 1] ^= 0xff;
    sha1(expected, piece_length, mi.piece_hashes);

    storage_t *store = storage_open(&mi, outdir);
    assert(store);
    piece_mgr_t *pm = piece_mgr_create(&mi, store);
    assert(pm);

    for (uint32_t block = 0; block < 4; block++) {
        piece_mgr_mark_block_requested(pm, 0, block);
        int result = piece_mgr_got_block(pm, 0, block * BLOCK_SIZE,
                                         corrupt + block * BLOCK_SIZE, BLOCK_SIZE);
        assert(result == (block == 3 ? 0 : 1));
    }
    assert(pm->slots[0].state == PS_EMPTY);
    assert(pm->completed_bytes == 0);
    assert(pm->slots[0].num_blocks_done == 0);
    for (uint32_t block = 0; block < 4; block++) {
        assert(!piece_mgr_has_block(pm, 0, block));
        assert(!piece_mgr_block_requested(pm, 0, block));
    }

    assert(piece_mgr_got_block(pm, 0, 0, expected, BLOCK_SIZE - 1) == -1);
    for (uint32_t block = 0; block < 4; block++) {
        int result = piece_mgr_got_block(pm, 0, block * BLOCK_SIZE,
                                         expected + block * BLOCK_SIZE, BLOCK_SIZE);
        assert(result == (block == 3 ? 2 : 1));
    }
    assert(pm->num_done == 1);
    assert(pm->completed_bytes == (uint64_t)piece_length);

    piece_mgr_destroy(pm);
    storage_close(store);
    free(corrupt);
    free(expected);
    free_test_metainfo(&mi);
    cleanup_output(outdir, "retry.bin");
}

static void test_final_verify_requeues_disk_corruption(void) {
    const size_t piece_length = 2 * BLOCK_SIZE;
    char outdir[] = "/tmp/pipensx-piece-disk-XXXXXX";
    char path[512];
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "disk.bin", piece_length, piece_length);

    uint8_t *expected = (uint8_t*)malloc(piece_length);
    assert(expected);
    fill_pattern(expected, piece_length, 41);
    sha1(expected, piece_length, mi.piece_hashes);

    storage_t *store = storage_open(&mi, outdir);
    assert(store);
    piece_mgr_t *pm = piece_mgr_create(&mi, store);
    assert(pm);

    assert(piece_mgr_got_block(pm, 0, 0, expected, BLOCK_SIZE) == 1);
    assert(piece_mgr_got_block(pm, 0, BLOCK_SIZE,
                               expected + BLOCK_SIZE, BLOCK_SIZE) == 2);
    assert(pm->completed_bytes == (uint64_t)piece_length);
    assert(piece_mgr_verify_all(pm));

    snprintf(path, sizeof(path), "%s/%s", outdir, "disk.bin");
    FILE *f = fopen(path, "r+b");
    assert(f);
    assert(fseek(f, BLOCK_SIZE + 10, SEEK_SET) == 0);
    assert(fputc(expected[BLOCK_SIZE + 10] ^ 0xff, f) != EOF);
    assert(fflush(f) == 0);
    fclose(f);

    assert(!piece_mgr_verify_all(pm));
    assert(pm->num_done == 0);
    assert(pm->completed_bytes == 0);
    assert(pm->slots[0].state == PS_EMPTY);
    assert(!bf_has(pm->have_bf, 0));

    assert(piece_mgr_got_block(pm, 0, 0, expected, BLOCK_SIZE) == 1);
    assert(piece_mgr_got_block(pm, 0, BLOCK_SIZE,
                               expected + BLOCK_SIZE, BLOCK_SIZE) == 2);
    assert(pm->completed_bytes == (uint64_t)piece_length);
    assert(piece_mgr_verify_all(pm));

    piece_mgr_destroy(pm);
    storage_close(store);
    free(expected);
    free_test_metainfo(&mi);
    cleanup_output(outdir, "disk.bin");
}

static void test_existing_piece_scan_restores_progress(void) {
    const size_t piece_length = 2 * BLOCK_SIZE;
    char outdir[] = "/tmp/pipensx-piece-resume-XXXXXX";
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "resume.bin", piece_length,
                              piece_length * 2);

    uint8_t *expected = (uint8_t*)malloc(piece_length * 2);
    assert(expected);
    fill_pattern(expected, piece_length * 2, 73);
    sha1(expected, piece_length, mi.piece_hashes);
    sha1(expected + piece_length, piece_length, mi.piece_hashes + 20);

    storage_t *store = storage_open(&mi, outdir);
    assert(store);
    assert(storage_write(store, 0, expected, piece_length));
    assert(storage_write(store, piece_length, expected + piece_length,
                         piece_length));
    assert(storage_flush(store));

    piece_mgr_t *pm = piece_mgr_create(&mi, store);
    assert(pm);
    assert(piece_mgr_check_existing(pm, 0) == 1);
    assert(pm->completed_bytes == (uint64_t)piece_length);
    assert(piece_mgr_check_existing(pm, 1) == 1);
    assert(pm->num_done == 2);
    assert(pm->completed_bytes == (uint64_t)piece_length * 2);

    expected[piece_length + 1] ^= 0xff;
    assert(storage_write(store, piece_length, expected + piece_length,
                         piece_length));
    assert(storage_flush(store));
    assert(piece_mgr_check_existing(pm, 1) == 0);
    assert(pm->num_done == 1);
    assert(pm->completed_bytes == (uint64_t)piece_length);
    assert(pm->slots[1].state == PS_EMPTY);

    piece_mgr_destroy(pm);
    storage_close(store);
    free(expected);
    free_test_metainfo(&mi);
    cleanup_output(outdir, "resume.bin");
}

struct sink_capture {
    uint8_t *data;
    size_t size;
    size_t written;
};

static int capture_sink(void *user, uint32_t file_index, int64_t file_offset,
                        const uint8_t *data, size_t size) {
    struct sink_capture *capture = (struct sink_capture*)user;
    assert(file_index == 0);
    assert(file_offset == (int64_t)capture->written);
    assert(capture->written + size <= capture->size);
    memcpy(capture->data + capture->written, data, size);
    capture->written += size;
    return 1;
}

static void test_stream_sink_piece_is_verified_without_disk_readback(void) {
    const size_t piece_length = 2 * BLOCK_SIZE;
    char outdir[] = "/tmp/pipensx-piece-sink-XXXXXX";
    char path[512];
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_single_file_metainfo(&mi, "package.nsp", piece_length, piece_length);
    uint8_t *expected = (uint8_t*)malloc(piece_length);
    uint8_t *captured = (uint8_t*)calloc(piece_length, 1);
    assert(expected && captured);
    fill_pattern(expected, piece_length, 113);
    sha1(expected, piece_length, mi.piece_hashes);

    struct sink_capture capture = {captured, piece_length, 0};
    storage_file_config_t config = {
        STORAGE_FILE_SINK, capture_sink, &capture
    };
    storage_t *store = storage_open_ex(&mi, outdir, &config);
    assert(store);
    piece_mgr_t *pm = piece_mgr_create(&mi, store);
    assert(pm);

    assert(piece_mgr_got_block(pm, 0, 0, expected, BLOCK_SIZE) == 1);
    assert(piece_mgr_got_block(pm, 0, BLOCK_SIZE,
                               expected + BLOCK_SIZE, BLOCK_SIZE) == 2);
    assert(pm->completed_bytes == (uint64_t)piece_length);
    assert(capture.written == piece_length);
    assert(memcmp(captured, expected, piece_length) == 0);
    assert(piece_mgr_verify_all(pm));
    assert(bf_has(pm->have_bf, 0));
    assert(!bf_has(pm->available_bf, 0));

    snprintf(path, sizeof(path), "%s/%s", outdir, "package.nsp");
    assert(access(path, F_OK) != 0);

    piece_mgr_destroy(pm);
    storage_close(store);
    free(captured);
    free(expected);
    free_test_metainfo(&mi);
    rmdir(outdir);
}

static void init_long_disk_path_metainfo(metainfo_t *mi) {
    memset(mi, 0, sizeof(*mi));
    memset(mi->name, 'A', sizeof(mi->name) - 1);
    mi->name[sizeof(mi->name) - 1] = '\0';
    mi->piece_length = BLOCK_SIZE;
    mi->total_length = 1;
    mi->num_pieces = 1;
    mi->piece_hashes = (uint8_t*)calloc(1, 20);
    mi->num_files = 1;
    mi->files = (mi_file_t*)calloc(1, sizeof(*mi->files));
    mi->is_multi = 1;
    assert(mi->piece_hashes);
    assert(mi->files);
    memset(mi->files[0].path, 'b', 230);
    mi->files[0].path[230] = '/';
    snprintf(mi->files[0].path + 231, sizeof(mi->files[0].path) - 231,
             "payload.bin");
    mi->files[0].length = 1;
}

static void test_long_disk_path_uses_short_fallback(void) {
    char outdir[] = "/tmp/pipensx-long-path-XXXXXX";
    char path[512];
    uint8_t value = 0x5a;
    uint8_t actual = 0;
    assert(mkdtemp(outdir));

    metainfo_t mi;
    init_long_disk_path_metainfo(&mi);

    storage_t *store = storage_open(&mi, outdir);
    assert(store);
    assert(storage_write(store, 0, &value, 1));
    assert(storage_read(store, 0, &actual, 1) == 1);
    assert(actual == value);
    storage_close(store);

    snprintf(path, sizeof(path), "%s/_files/000000_payload.bin", outdir);
    assert(access(path, F_OK) == 0);
    unlink(path);
    snprintf(path, sizeof(path), "%s/_files", outdir);
    rmdir(path);
    rmdir(outdir);
    free_test_metainfo(&mi);
}

static void test_metainfo_path_safety(void) {
    assert(metainfo_path_is_safe("file.bin"));
    assert(metainfo_path_is_safe("folder/file.bin"));
    assert(!metainfo_path_is_safe(""));
    assert(!metainfo_path_is_safe("/absolute"));
    assert(!metainfo_path_is_safe("../escape"));
    assert(!metainfo_path_is_safe("folder/../escape"));
    assert(!metainfo_path_is_safe("folder//file"));
    assert(!metainfo_path_is_safe("folder\\file"));
}

static void test_strict_order_advances_past_lookahead_window(void) {
    metainfo_t mi;
    init_single_file_metainfo(&mi, "ordered.bin", BLOCK_SIZE,
                              40 * BLOCK_SIZE);
    piece_mgr_t *pm = piece_mgr_create_ex(&mi, NULL, 1, NULL, 0);
    assert(pm);

    for (uint32_t i = 0; i < 32; i++)
        pm->slots[i].state = PS_DONE;

    uint8_t peer_bf[5] = {0};
    bf_set(peer_bf, 32);
    assert(piece_mgr_pick(pm, peer_bf, sizeof(peer_bf)) == 32);

    piece_mgr_destroy(pm);
    free_test_metainfo(&mi);
}

static int allow_pieces_below_limit(void *user, uint32_t piece) {
    uint32_t limit = *(uint32_t*)user;
    return piece < limit;
}

static void test_strict_order_stops_at_request_gate(void) {
    metainfo_t mi;
    init_single_file_metainfo(&mi, "ordered.bin", BLOCK_SIZE,
                              40 * BLOCK_SIZE);
    piece_mgr_t *pm = piece_mgr_create_ex(&mi, NULL, 1, NULL, 0);
    assert(pm);

    for (uint32_t i = 0; i < 32; i++)
        pm->slots[i].state = PS_DONE;

    uint32_t limit = 32;
    pm->request_allowed = allow_pieces_below_limit;
    pm->request_allowed_user = &limit;

    uint8_t peer_bf[5] = {0};
    bf_set(peer_bf, 32);
    bf_set(peer_bf, 33);
    assert(piece_mgr_pick(pm, peer_bf, sizeof(peer_bf)) == (uint32_t)-1);

    limit = 34;
    assert(piece_mgr_pick(pm, peer_bf, sizeof(peer_bf)) == 32);

    piece_mgr_destroy(pm);
    free_test_metainfo(&mi);
}

static void test_request_reference_counts(void) {
    metainfo_t mi;
    init_single_file_metainfo(&mi, "requests.bin", BLOCK_SIZE, BLOCK_SIZE);
    piece_mgr_t *pm = piece_mgr_create(&mi, NULL);
    assert(pm);

    assert(piece_mgr_block_request_count(pm, 0, 0) == 0);
    piece_mgr_mark_block_requested(pm, 0, 0);
    piece_mgr_mark_block_requested(pm, 0, 0);
    assert(piece_mgr_block_request_count(pm, 0, 0) == 2);
    piece_mgr_clear_block_requested(pm, 0, 0);
    assert(piece_mgr_block_request_count(pm, 0, 0) == 1);
    piece_mgr_clear_all_block_requests(pm, 0, 0);
    assert(!piece_mgr_block_requested(pm, 0, 0));

    piece_mgr_destroy(pm);
    free_test_metainfo(&mi);
}

static void test_strict_order_prefers_requestable_pending_piece(void) {
    metainfo_t mi;
    init_single_file_metainfo(&mi, "pending.bin", BLOCK_SIZE,
                              3 * BLOCK_SIZE);
    piece_mgr_t *pm = piece_mgr_create_ex(&mi, NULL, 1, NULL, 0);
    assert(pm);
    piece_mgr_set_strict_policy(pm, 2, 1);

    pm->slots[0].state = PS_PENDING;
    uint8_t peer_bf[1] = {0};
    bf_set(peer_bf, 0);
    bf_set(peer_bf, 1);
    assert(piece_mgr_pick(pm, peer_bf, sizeof(peer_bf)) == 0);

    piece_mgr_mark_block_requested(pm, 0, 0);
    assert(piece_mgr_pick(pm, peer_bf, sizeof(peer_bf)) == 1);

    piece_mgr_destroy(pm);
    free_test_metainfo(&mi);
}

int main(void) {
    test_large_piece_and_short_last_piece();
    test_hash_mismatch_resets_all_blocks();
    test_final_verify_requeues_disk_corruption();
    test_existing_piece_scan_restores_progress();
    test_stream_sink_piece_is_verified_without_disk_readback();
    test_long_disk_path_uses_short_fallback();
    test_metainfo_path_safety();
    test_strict_order_advances_past_lookahead_window();
    test_strict_order_stops_at_request_gate();
    test_request_reference_counts();
    test_strict_order_prefers_requestable_pending_piece();
    puts("piece tests passed");
    return 0;
}

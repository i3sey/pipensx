#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../core/metainfo.h"

typedef struct storage storage_t;

typedef enum {
    STORAGE_FILE_DISK = 0,
    STORAGE_FILE_SINK = 1,
    STORAGE_FILE_SKIP = 2,
} storage_file_mode_t;

typedef int (*storage_sink_fn)(void *user, uint32_t file_index,
                               int64_t file_offset,
                               const uint8_t *data, size_t len);

typedef struct {
    storage_file_mode_t mode;
    storage_sink_fn sink;
    void *user;
    /*
     * IMPROVEMENT_PLAN F-B: prefix of a SINK file already consumed by a
     * previous session's installer (from the install journal). Pieces that
     * lie entirely below this mark are reported as skipped (no re-download);
     * a piece straddling the mark is re-downloaded and only its tail at/after
     * the mark is delivered to the sink. 0 = plain sink behaviour.
     */
    uint64_t ready_bytes;
    /*
     * Test hook: skip the >4 GiB probe and store as a DBI split folder
     * (applies only to files above the FAT32 ceiling). 0 in production.
     */
    int force_split;
} storage_file_config_t;

/*
 * Open (create) output files for a torrent.
 * outdir: base directory (created if absent).
 * Returns handle or NULL on error.
 */
storage_t *storage_open(const metainfo_t *mi, const char *outdir);
storage_t *storage_open_ex(const metainfo_t *mi, const char *outdir,
                           const storage_file_config_t *configs);

/* Locate an existing on-disk torrent file, including the long-path fallback. */
int storage_locate_file_path(const metainfo_t *mi, const char *outdir,
                             uint32_t file_index, char *out, size_t out_size);

/*
 * Write data at the absolute torrent byte offset.
 * Returns 1 on success, 0 on error.
 */
int storage_write(storage_t *s, int64_t offset, const uint8_t *data, size_t len);

/* Flush all output files. Returns 1 on success, 0 on error. */
int storage_flush(storage_t *s);

/*
 * Read data at the absolute torrent byte offset (for seeding / verify).
 * Returns bytes actually read, or -1 on error.
 */
int storage_read(storage_t *s, int64_t offset, uint8_t *data, size_t len);

/* True when the complete range is backed by ordinary files. */
int storage_range_readable(storage_t *s, int64_t offset, size_t len);

/*
 * True when the complete range is already processed: SKIP files plus the
 * consumed (ready_bytes) prefix of SINK files.
 */
int storage_range_skipped(storage_t *s, int64_t offset, size_t len);

/*
 * Called when the torrent completes (all pieces verified): marks every
 * finished split folder with the concatenation attribute so HOS reads it
 * as one file (Switch only; no-op elsewhere). Idempotent.
 */
void storage_finalize(storage_t *s);

const char *storage_error(storage_t *s);

/* Record a fatal error on this storage handle (first error wins). */
void storage_set_error(storage_t *s, const char *msg);

/* Last error from a failed storage_open/storage_open_ex (handle was freed). */
const char *storage_open_error(void);

void storage_close(storage_t *s);

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
} storage_file_config_t;

/*
 * Open (create) output files for a torrent.
 * outdir: base directory (created if absent).
 * Returns handle or NULL on error.
 */
storage_t *storage_open(const metainfo_t *mi, const char *outdir);
storage_t *storage_open_ex(const metainfo_t *mi, const char *outdir,
                           const storage_file_config_t *configs);

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

const char *storage_error(storage_t *s);

void storage_close(storage_t *s);

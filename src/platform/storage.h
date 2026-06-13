#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../core/metainfo.h"

typedef struct storage storage_t;

/*
 * Open (create) output files for a torrent.
 * outdir: base directory (created if absent).
 * Returns handle or NULL on error.
 */
storage_t *storage_open(const metainfo_t *mi, const char *outdir);

/*
 * Write data at the absolute torrent byte offset.
 * Returns 1 on success, 0 on error.
 */
int storage_write(storage_t *s, int64_t offset, const uint8_t *data, size_t len);

/*
 * Read data at the absolute torrent byte offset (for seeding / verify).
 * Returns bytes actually read, or -1 on error.
 */
int storage_read(storage_t *s, int64_t offset, uint8_t *data, size_t len);

void storage_close(storage_t *s);

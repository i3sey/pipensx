#pragma once
#include <stddef.h>
#include <stdint.h>

#define MAX_TRACKERS  32
#define MAX_FILES     512
#define MAX_NAME_LEN  256

typedef struct {
    char    path[MAX_NAME_LEN]; /* relative path, '/' separated */
    int64_t length;
    int64_t offset; /* byte offset in the torrent's flat data space */
} mi_file_t;

typedef struct {
    /* Identity */
    uint8_t  info_hash[20];
    char     name[MAX_NAME_LEN];

    /* Piece info */
    int64_t  piece_length;
    uint32_t num_pieces;
    uint8_t *piece_hashes; /* num_pieces * 20 bytes, heap-allocated */

    /* Files */
    int      is_multi;
    int64_t  total_length;
    uint32_t num_files;
    mi_file_t *files; /* heap-allocated; for single-file [0] == the file */

    /* Trackers (announce-list flattened) */
    uint32_t num_trackers;
    char     trackers[MAX_TRACKERS][512];
} metainfo_t;

/*
 * Parse a .torrent file from memory.
 * Returns 1 on success, 0 on error.
 * Caller must call metainfo_free() when done.
 */
int metainfo_parse(const uint8_t *data, size_t len, metainfo_t *mi);
void metainfo_free(metainfo_t *mi);

/* Load from file path */
int metainfo_load(const char *path, metainfo_t *mi);

/* Returns non-zero when a torrent path is safe to append to an output dir. */
int metainfo_path_is_safe(const char *path);

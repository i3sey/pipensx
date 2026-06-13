#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../core/metainfo.h"
#include "../platform/storage.h"

#define BLOCK_SIZE  (16*1024)  /* 16 KB — standard BitTorrent block */

typedef enum {
    PS_EMPTY    = 0,
    PS_PENDING  = 1,  /* requested from at least one peer */
    PS_DONE     = 2   /* verified and written */
} piece_state_t;

typedef struct {
    piece_state_t state;
    uint8_t      *buf;         /* piece_length bytes, NULL until first request */
    uint8_t      *have_blocks; /* bitmap of received 16KB blocks */
    uint32_t      num_blocks;
    uint32_t      num_blocks_done;
} piece_slot_t;

typedef struct {
    const metainfo_t *mi;
    storage_t        *store;
    piece_slot_t     *slots;  /* [num_pieces] */
    uint32_t          num_done;
    uint32_t          num_pieces;
    uint8_t          *have_bf; /* bitfield of completed pieces (num_pieces bits) */
} piece_mgr_t;

piece_mgr_t *piece_mgr_create(const metainfo_t *mi, storage_t *store);
void         piece_mgr_destroy(piece_mgr_t *pm);

/* Mark a block as requested by a peer (sets PS_PENDING) */
void piece_mgr_mark_pending(piece_mgr_t *pm, uint32_t idx);

/*
 * Receive a block.  Returns:
 *   2 = piece complete and verified (have_bf updated)
 *   1 = block stored, piece not yet complete
 *   0 = hash mismatch (piece reset)
 *  -1 = error (bad params etc.)
 */
int piece_mgr_got_block(piece_mgr_t *pm, uint32_t idx, uint32_t offset,
                        const uint8_t *data, uint32_t len);

/* Verify one completed piece by reading it from storage. */
int piece_mgr_verify_piece(piece_mgr_t *pm, uint32_t idx);

/*
 * Flush and verify every completed piece from storage.
 * Corrupt pieces are reset for downloading again.
 * Returns 1 when all pieces verify, 0 otherwise.
 */
int piece_mgr_verify_all(piece_mgr_t *pm);

/* Returns non-zero when the block has already been received. */
int piece_mgr_has_block(const piece_mgr_t *pm, uint32_t idx, uint32_t block);

/*
 * Pick the next piece index to request from a peer.
 * peer_bf = peer's have-bitfield (same format as have_bf).
 * Returns piece index or (uint32_t)-1 if nothing to request.
 */
uint32_t piece_mgr_pick(const piece_mgr_t *pm,
                        const uint8_t *peer_bf, uint32_t bf_bytes);

/* Size of the last (possibly short) piece */
int64_t piece_len(const piece_mgr_t *pm, uint32_t idx);

/* Number of 16KB blocks in a piece */
uint32_t piece_num_blocks(const piece_mgr_t *pm, uint32_t idx);

/* bf_has / bf_set defined in util.h */

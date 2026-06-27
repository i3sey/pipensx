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
    uint8_t      *request_counts; /* number of peers requesting each block */
    uint32_t      num_blocks;
    uint32_t      num_blocks_done;
} piece_slot_t;

typedef struct {
    const metainfo_t *mi;
    storage_t        *store;
    piece_slot_t     *slots;  /* [num_pieces] */
    uint32_t          num_done;
    uint32_t          num_pieces;
    uint64_t          completed_bytes;
    uint8_t          *have_bf; /* bitfield of completed pieces (num_pieces bits) */
    uint8_t          *available_bf; /* pieces retained on disk for upload */
    int               strict_order;
    uint32_t          strict_order_lookahead;
    int               strict_fill_pending_first;
    uint32_t         *piece_order;
    uint32_t          piece_order_count;
    int             (*request_allowed)(void *user, uint32_t piece);
    void             *request_allowed_user;
} piece_mgr_t;

piece_mgr_t *piece_mgr_create(const metainfo_t *mi, storage_t *store);
piece_mgr_t *piece_mgr_create_ex(const metainfo_t *mi, storage_t *store,
                                 int strict_order,
                                 const uint32_t *piece_order,
                                 uint32_t piece_order_count);
void         piece_mgr_destroy(piece_mgr_t *pm);
void         piece_mgr_set_strict_policy(piece_mgr_t *pm,
                                         uint32_t lookahead,
                                         int fill_pending_first);

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
 * Check a piece already present in storage and update the have bitfield.
 * Returns 1 when valid, 0 when absent/corrupt, and -1 on invalid arguments.
 */
int piece_mgr_check_existing(piece_mgr_t *pm, uint32_t idx);

/*
 * Flush and verify every completed piece from storage.
 * Corrupt pieces are reset for downloading again.
 * Returns 1 when all pieces verify, 0 otherwise.
 */
int piece_mgr_verify_all(piece_mgr_t *pm);

/* Returns non-zero when the block has already been received. */
int piece_mgr_has_block(const piece_mgr_t *pm, uint32_t idx, uint32_t block);
int piece_mgr_block_requested(const piece_mgr_t *pm, uint32_t idx,
                              uint32_t block);
uint32_t piece_mgr_block_request_count(const piece_mgr_t *pm, uint32_t idx,
                                       uint32_t block);
void piece_mgr_mark_block_requested(piece_mgr_t *pm, uint32_t idx,
                                    uint32_t block);
void piece_mgr_clear_block_requested(piece_mgr_t *pm, uint32_t idx,
                                     uint32_t block);
void piece_mgr_clear_all_block_requests(piece_mgr_t *pm, uint32_t idx,
                                        uint32_t block);

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

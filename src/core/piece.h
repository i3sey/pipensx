#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "../core/metainfo.h"
#include "../platform/storage.h"

#define BLOCK_SIZE  (16*1024)  /* 16 KB — standard BitTorrent block */
/* Completed pieces hand their buffer back to a small recycle pool instead of
   free(): piece buffers are piece_length (typically MiBs) and churn at piece
   rate, so pooling avoids a large alloc+zero per piece and heap
   fragmentation from the constant same-size malloc/free. */
#define PIECE_BUF_POOL_MAX 4

typedef enum {
    PS_EMPTY    = 0,
    PS_PENDING  = 1,  /* requested from at least one peer */
    PS_DONE     = 2,  /* verified and written */
    PS_HASHING  = 3   /* all blocks received; SHA-1 running on the hash
                         worker; buf detached from the slot until the
                         result is applied on the torrent thread */
} piece_state_t;

typedef struct {
    piece_state_t state;
    uint8_t      *buf;         /* piece_length bytes, NULL until first request */
    uint8_t      *have_blocks; /* bitmap of received 16KB blocks */
    uint8_t      *request_counts; /* number of peers requesting each block */
    uint32_t      num_blocks;
    uint32_t      num_blocks_done;
} piece_slot_t;

/* Async hash pipeline: one FIFO ring shared as job queue and result queue.
   The torrent thread enqueues at tail (piece buf detached from its slot,
   slot -> PS_HASHING); the worker hashes entries strictly in order and flags
   them done; the torrent thread applies done entries strictly from head
   (storage_write + mark DONE, or reset on mismatch), so pieces are written in
   exactly the completion order they would have been written in with inline
   hashing. Small on purpose: bounds detached-buffer RAM to
   PIECE_HASH_QUEUE_MAX x piece_length; a full queue back-pressures the
   torrent thread for at most one in-flight hash. */
#define PIECE_HASH_QUEUE_MAX 2

typedef struct {
    uint32_t idx;
    uint8_t *buf;    /* detached piece buffer; worker-owned until applied */
    uint32_t plen;
    int      done;   /* worker finished hashing (guarded by hash_mutex) */
    int      ok;     /* digest matched expected (valid once done) */
} piece_hash_job_t;

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
    uint8_t          *verify_buf; /* piece_length scratch, lazily allocated */
    uint8_t          *buf_pool[PIECE_BUF_POOL_MAX]; /* recycled piece buffers */
    uint32_t          buf_pool_count;
    /* Position in piece_order (or the identity order) of the first piece that
       is not DONE. Advanced when pieces complete, rewound by reset_piece, so
       the strict picker and head-piece queries start at the download frontier
       instead of rescanning every completed piece. */
    uint32_t          order_cursor;
    uint32_t         *order_pos;  /* piece index -> piece_order position */
    int             (*request_allowed)(void *user, uint32_t piece);
    void             *request_allowed_user;
    /* ---- async hash worker ----
       hash_q / hash_q_head / hash_q_count / hash_q_hashed / hash_shutdown
       are guarded by hash_mutex. hash_cond wakes the worker (new job or
       shutdown); hash_done_cond wakes the torrent thread (flush and
       queue-full backpressure). hash_ok / hash_started / hash_result_cb*
       are torrent-thread-only. Job bufs are exclusively worker-owned
       between enqueue and apply; the mutex hand-off publishes their
       contents. */
    piece_hash_job_t hash_q[PIECE_HASH_QUEUE_MAX];
    uint32_t         hash_q_head;    /* oldest occupied entry */
    uint32_t         hash_q_count;   /* occupied entries */
    uint32_t         hash_q_hashed;  /* entries the worker has finished */
    pthread_t        hash_thread;
    pthread_mutex_t  hash_mutex;
    pthread_cond_t   hash_cond;
    pthread_cond_t   hash_done_cond;
    int              hash_ok;        /* mutex/conds initialized */
    int              hash_started;   /* worker thread spawned (lazy) */
    int              hash_shutdown;
    /* Reports each applied async result on the torrent thread, at apply
       time, from whichever path applied it (drain, flush, backpressure).
       status reuses got_block codes: 2 verified+written, 0 mismatch
       (piece reset), -1 write error (piece reset). Optional. */
    void           (*hash_result_cb)(void *user, uint32_t idx, int status);
    void            *hash_result_user;
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
 *   2 = piece complete and verified inline (worker unavailable; have_bf
 *       updated)
 *   1 = block stored. On the last block of a piece the slot moves to
 *       PS_HASHING and verification completes asynchronously in
 *       piece_mgr_drain_hash_results / piece_mgr_hash_flush.
 *   0 = inline hash mismatch (piece reset)
 *  -1 = error (bad params etc.)
 */
int piece_mgr_got_block(piece_mgr_t *pm, uint32_t idx, uint32_t offset,
                        const uint8_t *data, uint32_t len);

/* Apply every finished async hash result (FIFO): verified pieces are
   written and marked DONE, mismatches reset. Non-blocking; no-op when the
   worker was never started. Torrent thread only. */
void piece_mgr_drain_hash_results(piece_mgr_t *pm);

/* Block until every queued hash job has finished, then apply all results.
   Used before the fast-resume have_bf snapshot, by piece_mgr_destroy and
   piece_mgr_verify_all, and by tests. Torrent thread only. */
void piece_mgr_hash_flush(piece_mgr_t *pm);

/* Verify one completed piece by reading it from storage. */
int piece_mgr_verify_piece(piece_mgr_t *pm, uint32_t idx);

/*
 * Check a piece already present in storage and update the have bitfield.
 * Returns 1 when valid, 0 when absent/corrupt, and -1 on invalid arguments.
 */
int piece_mgr_check_existing(piece_mgr_t *pm, uint32_t idx);

/*
 * Fast resume: mark the pieces set in a previously saved have-bitfield as
 * DONE without hashing them. bf_len must be exactly (num_pieces+7)/8 or the
 * call is a no-op. Bits over ranges that are neither skipped nor readable
 * (e.g. an unconsumed SINK range after an install-journal regression) are
 * dropped so those pieces download again.
 */
void piece_mgr_preset_have(piece_mgr_t *pm, const uint8_t *bf,
                           uint32_t bf_len);

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

/* First piece in download order that is not yet DONE, or UINT32_MAX when the
   torrent is complete. O(1) thanks to the maintained order cursor. */
uint32_t piece_mgr_head_piece(const piece_mgr_t *pm);

/* Size of the last (possibly short) piece */
int64_t piece_len(const piece_mgr_t *pm, uint32_t idx);

/* Number of 16KB blocks in a piece */
uint32_t piece_num_blocks(const piece_mgr_t *pm, uint32_t idx);

/* bf_has / bf_set defined in util.h */

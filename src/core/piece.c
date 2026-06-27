#include "piece.h"
#include "sha1.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STRICT_ORDER_LOOKAHEAD 32

int64_t piece_len(const piece_mgr_t *pm, uint32_t idx) {
    if (idx + 1 < pm->num_pieces)
        return pm->mi->piece_length;
    /* Last piece */
    int64_t rem = pm->mi->total_length - (int64_t)idx * pm->mi->piece_length;
    return rem > 0 ? rem : 0;
}

uint32_t piece_num_blocks(const piece_mgr_t *pm, uint32_t idx) {
    int64_t plen = piece_len(pm, idx);
    return (uint32_t)((plen + BLOCK_SIZE - 1) / BLOCK_SIZE);
}

static size_t block_bitmap_size(uint32_t num_blocks) {
    return (num_blocks + 7u) / 8u;
}

static int block_is_set(const piece_slot_t *sl, uint32_t block) {
    if (!sl->have_blocks || block >= sl->num_blocks) return 0;
    return (sl->have_blocks[block / 8] >> (block % 8)) & 1u;
}

static int request_allowed(const piece_mgr_t *pm, uint32_t idx) {
    if (!pm->request_allowed)
        return 1;
    return pm->request_allowed(pm->request_allowed_user, idx);
}

static void block_set(piece_slot_t *sl, uint32_t block) {
    sl->have_blocks[block / 8] |= (uint8_t)(1u << (block % 8));
}

static void reset_piece(piece_mgr_t *pm, uint32_t idx) {
    piece_slot_t *sl = &pm->slots[idx];
    if (sl->state == PS_DONE && pm->num_done > 0) {
        uint64_t len = (uint64_t)piece_len(pm, idx);
        pm->num_done--;
        pm->completed_bytes = pm->completed_bytes >= len
            ? pm->completed_bytes - len
            : 0;
        pm->have_bf[idx / 8] &= (uint8_t)~(1u << (7 - idx % 8));
        pm->available_bf[idx / 8] &=
            (uint8_t)~(1u << (7 - idx % 8));
    }
    if (sl->buf)
        memset(sl->buf, 0, (size_t)pm->mi->piece_length);
    memset(sl->have_blocks, 0, block_bitmap_size(sl->num_blocks));
    memset(sl->request_counts, 0, sl->num_blocks);
    sl->num_blocks_done = 0;
    sl->state = PS_EMPTY;
}

piece_mgr_t *piece_mgr_create_ex(const metainfo_t *mi, storage_t *store,
                                 int strict_order,
                                 const uint32_t *piece_order,
                                 uint32_t piece_order_count) {
    piece_mgr_t *pm = (piece_mgr_t*)calloc(1, sizeof(*pm));
    if (!pm) return NULL;
    pm->mi         = mi;
    pm->store      = store;
    pm->num_pieces = mi->num_pieces;
    pm->slots      = (piece_slot_t*)calloc(mi->num_pieces, sizeof(piece_slot_t));
    pm->have_bf    = (uint8_t*)calloc((mi->num_pieces + 7) / 8, 1);
    pm->available_bf = (uint8_t*)calloc((mi->num_pieces + 7) / 8, 1);
    pm->strict_order = strict_order;
    pm->strict_order_lookahead = STRICT_ORDER_LOOKAHEAD;
    if (piece_order && piece_order_count) {
        pm->piece_order = (uint32_t*)malloc(
            piece_order_count * sizeof(uint32_t));
        if (pm->piece_order) {
            memcpy(pm->piece_order, piece_order,
                   piece_order_count * sizeof(uint32_t));
            pm->piece_order_count = piece_order_count;
        }
    }
    if (!pm->slots || !pm->have_bf || !pm->available_bf ||
        (piece_order && piece_order_count && !pm->piece_order)) {
        free(pm->slots); free(pm->have_bf); free(pm->available_bf);
        free(pm->piece_order); free(pm);
        return NULL;
    }
    for (uint32_t i = 0; i < mi->num_pieces; i++) {
        pm->slots[i].num_blocks = piece_num_blocks(pm, i);
        size_t bitmap_size = block_bitmap_size(pm->slots[i].num_blocks);
        pm->slots[i].have_blocks = (uint8_t*)calloc(bitmap_size, 1);
        pm->slots[i].request_counts = (uint8_t*)calloc(
            pm->slots[i].num_blocks, 1);
        if (!pm->slots[i].have_blocks || !pm->slots[i].request_counts) {
            piece_mgr_destroy(pm);
            return NULL;
        }
    }
    return pm;
}

piece_mgr_t *piece_mgr_create(const metainfo_t *mi, storage_t *store) {
    return piece_mgr_create_ex(mi, store, 0, NULL, 0);
}

void piece_mgr_destroy(piece_mgr_t *pm) {
    if (!pm) return;
    for (uint32_t i = 0; i < pm->num_pieces; i++) {
        free(pm->slots[i].buf);
        free(pm->slots[i].have_blocks);
        free(pm->slots[i].request_counts);
    }
    free(pm->slots);
    free(pm->have_bf);
    free(pm->available_bf);
    free(pm->piece_order);
    free(pm);
}

void piece_mgr_set_strict_policy(piece_mgr_t *pm, uint32_t lookahead,
                                 int fill_pending_first) {
    if (!pm)
        return;
    pm->strict_order_lookahead = lookahead ? lookahead
                                           : STRICT_ORDER_LOOKAHEAD;
    pm->strict_fill_pending_first = fill_pending_first != 0;
}

void piece_mgr_mark_pending(piece_mgr_t *pm, uint32_t idx) {
    if (idx >= pm->num_pieces) return;
    piece_slot_t *sl = &pm->slots[idx];
    if (sl->state == PS_EMPTY) sl->state = PS_PENDING;
    if (!sl->buf) {
        sl->buf = (uint8_t*)calloc(1, (size_t)pm->mi->piece_length);
        /* ignore alloc failure — got_block checks */
    }
}

int piece_mgr_got_block(piece_mgr_t *pm, uint32_t idx, uint32_t offset,
                        const uint8_t *data, uint32_t len) {
    if (idx >= pm->num_pieces || !data) return -1;
    piece_slot_t *sl = &pm->slots[idx];
    if (sl->state == PS_DONE) return 1; /* already have it */

    int64_t plen = piece_len(pm, idx);
    if (offset % BLOCK_SIZE != 0 || (int64_t)offset >= plen) return -1;

    uint32_t blk = offset / BLOCK_SIZE;
    uint32_t expected_len = ((int64_t)offset + BLOCK_SIZE <= plen)
                          ? BLOCK_SIZE : (uint32_t)(plen - offset);
    if (blk >= sl->num_blocks || len != expected_len) return -1;

    if (!sl->buf) {
        sl->buf = (uint8_t*)calloc(1, (size_t)pm->mi->piece_length);
        if (!sl->buf) return -1;
    }
    sl->state = PS_PENDING;
    memcpy(sl->buf + offset, data, len);

    if (!block_is_set(sl, blk)) {
        block_set(sl, blk);
        sl->num_blocks_done++;
    }
    piece_mgr_clear_all_block_requests(pm, idx, blk);
    if (sl->num_blocks_done != sl->num_blocks)
        return 1; /* not yet complete */

    /* Verify SHA-1 */
    uint8_t digest[20];
    sha1(sl->buf, (size_t)plen, digest);
    const uint8_t *expected = pm->mi->piece_hashes + idx * 20;
    if (memcmp(digest, expected, 20) != 0) {
        log_msg("[piece] SHA1 MISMATCH piece %u — resetting\n", idx);
        reset_piece(pm, idx);
        return 0;
    }

    int64_t abs_off = (int64_t)idx * pm->mi->piece_length;
    if (!storage_write(pm->store, abs_off, sl->buf, (size_t)plen)) {
        const char *error = storage_error(pm->store);
        log_msg("[piece] write error piece %u: %s\n", idx,
                error[0] ? error : "storage_write failed");
        reset_piece(pm, idx);
        return -1;
    }

    sl->state = PS_DONE;
    bf_set(pm->have_bf, idx);
    if (storage_range_readable(pm->store, abs_off, (size_t)plen))
        bf_set(pm->available_bf, idx);
    pm->num_done++;
    pm->completed_bytes += (uint64_t)plen;
    log_msg("[piece] verified piece %u/%u\n", pm->num_done, pm->num_pieces);

    /* Free buffer — piece is written */
    free(sl->buf);
    sl->buf = NULL;
    return 2;
}

int piece_mgr_verify_piece(piece_mgr_t *pm, uint32_t idx) {
    if (!pm || idx >= pm->num_pieces) return 0;
    piece_slot_t *sl = &pm->slots[idx];
    if (sl->state != PS_DONE) return 0;

    int64_t plen = piece_len(pm, idx);
    int64_t abs_off = (int64_t)idx * pm->mi->piece_length;
    if (!storage_range_readable(pm->store, abs_off, (size_t)plen))
        return sl->state == PS_DONE;
    uint8_t *buf = (uint8_t*)malloc((size_t)plen);
    if (!buf) return 0;

    uint8_t digest[20];
    int valid = 1;
    if (storage_read(pm->store, abs_off, buf, (size_t)plen) != plen) {
        log_msg("[piece] final read error piece %u — resetting\n", idx);
        valid = 0;
    } else {
        sha1(buf, (size_t)plen, digest);
        if (memcmp(digest, pm->mi->piece_hashes + idx * 20, 20) != 0) {
            log_msg("[piece] final SHA1 MISMATCH piece %u — resetting\n", idx);
            valid = 0;
        }
    }
    free(buf);
    if (!valid) reset_piece(pm, idx);
    return valid;
}

int piece_mgr_check_existing(piece_mgr_t *pm, uint32_t idx) {
    if (!pm || idx >= pm->num_pieces)
        return -1;

    piece_slot_t *sl = &pm->slots[idx];
    int64_t plen = piece_len(pm, idx);
    int64_t abs_off = (int64_t)idx * pm->mi->piece_length;
    if (storage_range_skipped(pm->store, abs_off, (size_t)plen)) {
        if (sl->state != PS_DONE) {
            sl->state = PS_DONE;
            sl->num_blocks_done = sl->num_blocks;
            memset(sl->have_blocks, 0xff, block_bitmap_size(sl->num_blocks));
            bf_set(pm->have_bf, idx);
            pm->num_done++;
            pm->completed_bytes += (uint64_t)plen;
        }
        return 1;
    }
    uint8_t *buf = (uint8_t*)malloc((size_t)plen);
    if (!buf)
        return 0;

    uint8_t digest[20];
    int valid = storage_read(pm->store, abs_off, buf, (size_t)plen) == plen;
    if (valid) {
        sha1(buf, (size_t)plen, digest);
        valid = memcmp(digest, pm->mi->piece_hashes + idx * 20, 20) == 0;
    }
    free(buf);

    if (valid) {
        if (sl->state != PS_DONE) {
            sl->state = PS_DONE;
            sl->num_blocks_done = sl->num_blocks;
            memset(sl->have_blocks, 0xff, block_bitmap_size(sl->num_blocks));
            bf_set(pm->have_bf, idx);
            bf_set(pm->available_bf, idx);
            pm->num_done++;
            pm->completed_bytes += (uint64_t)plen;
        }
        return 1;
    }

    reset_piece(pm, idx);
    return 0;
}

int piece_mgr_verify_all(piece_mgr_t *pm) {
    if (!pm || !storage_flush(pm->store)) return 0;

    int all_valid = 1;
    for (uint32_t idx = 0; idx < pm->num_pieces; idx++) {
        if (!piece_mgr_verify_piece(pm, idx))
            all_valid = 0;
    }
    return all_valid && pm->num_done == pm->num_pieces;
}

int piece_mgr_has_block(const piece_mgr_t *pm, uint32_t idx, uint32_t block) {
    if (!pm || idx >= pm->num_pieces) return 0;
    return block_is_set(&pm->slots[idx], block);
}

int piece_mgr_block_requested(const piece_mgr_t *pm, uint32_t idx,
                              uint32_t block) {
    return piece_mgr_block_request_count(pm, idx, block) != 0;
}

uint32_t piece_mgr_block_request_count(const piece_mgr_t *pm, uint32_t idx,
                                       uint32_t block) {
    if (!pm || idx >= pm->num_pieces) return 0;
    const piece_slot_t *sl = &pm->slots[idx];
    if (!sl->request_counts || block >= sl->num_blocks) return 0;
    return sl->request_counts[block];
}

void piece_mgr_mark_block_requested(piece_mgr_t *pm, uint32_t idx,
                                    uint32_t block) {
    if (!pm || idx >= pm->num_pieces) return;
    piece_slot_t *sl = &pm->slots[idx];
    if (!sl->request_counts || block >= sl->num_blocks) return;
    if (sl->request_counts[block] != UINT8_MAX)
        sl->request_counts[block]++;
}

void piece_mgr_clear_block_requested(piece_mgr_t *pm, uint32_t idx,
                                     uint32_t block) {
    if (!pm || idx >= pm->num_pieces) return;
    piece_slot_t *sl = &pm->slots[idx];
    if (!sl->request_counts || block >= sl->num_blocks) return;
    if (sl->request_counts[block] > 0)
        sl->request_counts[block]--;
}

void piece_mgr_clear_all_block_requests(piece_mgr_t *pm, uint32_t idx,
                                        uint32_t block) {
    if (!pm || idx >= pm->num_pieces) return;
    piece_slot_t *sl = &pm->slots[idx];
    if (!sl->request_counts || block >= sl->num_blocks) return;
    sl->request_counts[block] = 0;
}

static int slot_has_requestable_block(const piece_slot_t *slot) {
    if (!slot || !slot->request_counts)
        return 0;
    for (uint32_t block = 0; block < slot->num_blocks; ++block) {
        if (!block_is_set(slot, block) && slot->request_counts[block] == 0)
            return 1;
    }
    return 0;
}

uint32_t piece_mgr_pick(const piece_mgr_t *pm,
                        const uint8_t *peer_bf, uint32_t bf_bytes) {
    if (pm->strict_order) {
        uint32_t count = pm->piece_order_count
                       ? pm->piece_order_count : pm->num_pieces;
        uint32_t unfinished = 0;
        uint32_t pending_candidate = (uint32_t)-1;
        uint32_t empty_candidate = (uint32_t)-1;
        uint32_t lookahead = pm->strict_order_lookahead
                           ? pm->strict_order_lookahead
                           : STRICT_ORDER_LOOKAHEAD;
        for (uint32_t n = 0; n < count; n++) {
            uint32_t i = pm->piece_order_count ? pm->piece_order[n] : n;
            if (i >= pm->num_pieces)
                continue;
            if (pm->slots[i].state == PS_DONE)
                continue;
            if (!request_allowed(pm, i))
                break;
            if (unfinished++ >= lookahead)
                break;
            if (i / 8 < bf_bytes && bf_has(peer_bf, i)) {
                if (pm->slots[i].state == PS_PENDING &&
                    slot_has_requestable_block(&pm->slots[i]) &&
                    pending_candidate == (uint32_t)-1) {
                    pending_candidate = i;
                    if (pm->strict_fill_pending_first)
                        return i;
                } else if (pm->slots[i].state == PS_EMPTY &&
                           empty_candidate == (uint32_t)-1) {
                    empty_candidate = i;
                    if (!pm->strict_fill_pending_first)
                        return i;
                }
            }
        }
        if (pending_candidate != (uint32_t)-1)
            return pending_candidate;
        if (empty_candidate != (uint32_t)-1)
            return empty_candidate;
        return (uint32_t)-1;
    }

    /* Rarest-first would be ideal, but for minimality we do sequential:
       find the first piece the peer has and we don't (not DONE, not PENDING). */
    for (uint32_t i = 0; i < pm->num_pieces; i++) {
        if (!request_allowed(pm, i)) continue;
        if (pm->slots[i].state != PS_EMPTY) continue;
        if (!bf_has(pm->have_bf, i)) {
            if (i / 8 < bf_bytes && bf_has(peer_bf, i))
                return i;
        }
    }
    /* Second pass: allow re-requesting PENDING pieces (from a different peer) */
    for (uint32_t i = 0; i < pm->num_pieces; i++) {
        if (!request_allowed(pm, i)) continue;
        if (pm->slots[i].state == PS_DONE) continue;
        if (!bf_has(pm->have_bf, i)) {
            if (i / 8 < bf_bytes && bf_has(peer_bf, i))
                return i;
        }
    }
    return (uint32_t)-1;
}

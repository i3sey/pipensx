#include "piece.h"
#include "sha1.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

piece_mgr_t *piece_mgr_create(const metainfo_t *mi, storage_t *store) {
    piece_mgr_t *pm = (piece_mgr_t*)calloc(1, sizeof(*pm));
    if (!pm) return NULL;
    pm->mi         = mi;
    pm->store      = store;
    pm->num_pieces = mi->num_pieces;
    pm->slots      = (piece_slot_t*)calloc(mi->num_pieces, sizeof(piece_slot_t));
    pm->have_bf    = (uint8_t*)calloc((mi->num_pieces + 7) / 8, 1);
    if (!pm->slots || !pm->have_bf) {
        free(pm->slots); free(pm->have_bf); free(pm);
        return NULL;
    }
    for (uint32_t i = 0; i < mi->num_pieces; i++)
        pm->slots[i].num_blocks = piece_num_blocks(pm, i);
    return pm;
}

void piece_mgr_destroy(piece_mgr_t *pm) {
    if (!pm) return;
    for (uint32_t i = 0; i < pm->num_pieces; i++)
        free(pm->slots[i].buf);
    free(pm->slots);
    free(pm->have_bf);
    free(pm);
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
    if (idx >= pm->num_pieces) return -1;
    piece_slot_t *sl = &pm->slots[idx];
    if (sl->state == PS_DONE) return 1; /* already have it */

    int64_t plen = piece_len(pm, idx);
    if ((int64_t)offset + (int64_t)len > plen) return -1;

    if (!sl->buf) {
        sl->buf = (uint8_t*)calloc(1, (size_t)pm->mi->piece_length);
        if (!sl->buf) return -1;
    }
    sl->state = PS_PENDING;
    memcpy(sl->buf + offset, data, len);

    /* Mark block bit */
    uint32_t blk = offset / BLOCK_SIZE;
    if (blk < 32) sl->have_blocks |= (1u << blk);

    /* Check if all blocks received */
    uint32_t total_blocks = sl->num_blocks;
    uint32_t expected_mask = (total_blocks < 32) ? ((1u << total_blocks) - 1) : 0xFFFFFFFFu;
    if ((sl->have_blocks & expected_mask) != expected_mask)
        return 1; /* not yet complete */

    /* Verify SHA-1 */
    uint8_t digest[20];
    sha1(sl->buf, (size_t)plen, digest);
    const uint8_t *expected = pm->mi->piece_hashes + idx * 20;
    if (memcmp(digest, expected, 20) != 0) {
        log_msg("[piece] SHA1 MISMATCH piece %u — resetting\n", idx);
        memset(sl->buf, 0, (size_t)pm->mi->piece_length);
        sl->have_blocks = 0;
        sl->state = PS_EMPTY;
        return 0;
    }

    /* Write to storage */
    int64_t abs_off = (int64_t)idx * pm->mi->piece_length;
    if (!storage_write(pm->store, abs_off, sl->buf, (size_t)plen)) {
        log_msg("[piece] write error piece %u\n", idx);
        return -1;
    }

    sl->state = PS_DONE;
    bf_set(pm->have_bf, idx);
    pm->num_done++;
    log_msg("[piece] verified piece %u/%u\n", pm->num_done, pm->num_pieces);

    /* Free buffer — piece is written */
    free(sl->buf);
    sl->buf = NULL;
    return 2;
}

uint32_t piece_mgr_pick(const piece_mgr_t *pm,
                        const uint8_t *peer_bf, uint32_t bf_bytes) {
    /* Rarest-first would be ideal, but for minimality we do sequential:
       find the first piece the peer has and we don't (not DONE, not PENDING). */
    for (uint32_t i = 0; i < pm->num_pieces; i++) {
        if (pm->slots[i].state != PS_EMPTY) continue;
        if (!bf_has(pm->have_bf, i)) {
            if (i / 8 < bf_bytes && bf_has(peer_bf, i))
                return i;
        }
    }
    /* Second pass: allow re-requesting PENDING pieces (from a different peer) */
    for (uint32_t i = 0; i < pm->num_pieces; i++) {
        if (pm->slots[i].state == PS_DONE) continue;
        if (!bf_has(pm->have_bf, i)) {
            if (i / 8 < bf_bytes && bf_has(peer_bf, i))
                return i;
        }
    }
    return (uint32_t)-1;
}

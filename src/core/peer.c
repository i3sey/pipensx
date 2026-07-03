#include "peer.h"
#include "bencode.h"
#include "util.h"
/* bf_set/bf_has come from util.h */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* --- buffered send ---
 * The socket is nonblocking: whatever the kernel does not accept immediately
 * is queued in p->sbuf and drained by peer_flush() when the torrent loop
 * gets POLLOUT for this peer. */
static int peer_send_raw(peer_t *p, const uint8_t *data, uint32_t len) {
    if (p->sbuf_len == 0 && len > 0) {
        ssize_t n = net_send(p->fd, data, len);
        if (n < 0) return 0;
        data += (size_t)n;
        len  -= (uint32_t)n;
    }
    if (len == 0) return 1;
    if ((size_t)p->sbuf_len + len > sizeof(p->sbuf)) {
        log_msg("[peer] send queue overflow (%u+%u)\n", p->sbuf_len, len);
        return 0;
    }
    memcpy(p->sbuf + p->sbuf_len, data, len);
    p->sbuf_len += len;
    return 1;
}

int peer_flush(peer_t *p) {
    if (p->sbuf_len == 0) return 1;
    ssize_t n = net_send(p->fd, p->sbuf, p->sbuf_len);
    if (n < 0) return 0;
    if (n > 0) {
        memmove(p->sbuf, p->sbuf + (size_t)n, p->sbuf_len - (uint32_t)n);
        p->sbuf_len -= (uint32_t)n;
    }
    return 1;
}

/* --- message framing --- */
static int send_msg(peer_t *p, const uint8_t *payload, uint32_t plen) {
    uint8_t hdr[4];
    hdr[0] = (plen>>24)&0xFF;
    hdr[1] = (plen>>16)&0xFF;
    hdr[2] = (plen>> 8)&0xFF;
    hdr[3] = (plen    )&0xFF;
    /* Reserve room for the whole frame up front: several callers ignore
       failures, so a partially queued frame must never reach the wire. */
    if ((size_t)p->sbuf_len + 4 + plen > sizeof(p->sbuf)) {
        if (!peer_flush(p) ||
            (size_t)p->sbuf_len + 4 + plen > sizeof(p->sbuf)) {
            log_msg("[peer] send queue full, dropping frame (%u+%u)\n",
                    p->sbuf_len, 4 + plen);
            return 0;
        }
    }
    if (!peer_send_raw(p, hdr, 4)) return 0;
    if (plen > 0) return peer_send_raw(p, payload, plen);
    return 1;
}

static int send_byte_msg(peer_t *p, uint8_t id) {
    return send_msg(p, &id, 1);
}

/* --- handshake --- */
int peer_send_handshake(peer_t *p, const peer_ctx_t *ctx) {
    uint8_t hs[BT_HANDSHAKE_LEN];
    hs[0] = 19;
    memcpy(hs+1, "BitTorrent protocol", 19);
    memset(hs+20, 0, 8);
    /* Set extension bit (byte 5, bit 4 from right = 0x10) and DHT bit (byte 7, bit 0) */
    hs[25] = 0x10;  /* extension protocol (BEP10) */
    hs[27] = 0x01;  /* DHT */
    memcpy(hs+28, ctx->info_hash, 20);
    memcpy(hs+48, ctx->peer_id,   20);
    p->state = PS_HANDSHAKE;
    return peer_send_raw(p, hs, BT_HANDSHAKE_LEN);
}

/* --- allocate --- */
peer_t *peer_create(socket_t fd, struct sockaddr_in addr, const peer_ctx_t *ctx) {
    peer_t *p = (peer_t*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->fd       = fd;
    p->addr     = addr;
    p->state    = PS_CONNECTING;
    p->am_choked    = 1;
    p->peer_choked  = 1;
    snprintf(p->addr_str, sizeof(p->addr_str), "%s:%u",
             inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    p->bf_bytes = ctx->bf_bytes;
    p->bitfield = (uint8_t*)calloc(1, ctx->bf_bytes);
    p->connect_time_ms = now_ms();
    p->last_recv_ms    = p->connect_time_ms;
    return p;
}

void peer_destroy(peer_t *p) {
    if (!p) return;
    net_close(p->fd);
    free(p->bitfield);
    free(p->pex_buf);
    free(p);
}

/* --- BEP10 extension handshake --- */
int peer_send_ext_handshake(peer_t *p, uint16_t listen_port) {
    /* Build: d8:completei0e1:md6:ut_pexi1ee1:p<port>e */
    uint8_t buf[256];
    size_t off = 0;
    buf[off++] = MSG_EXTENDED; /* 20 */
    buf[off++] = EXT_HANDSHAKE_ID; /* 0 = handshake */
    /* bencode dict */
    buf[off++] = 'd';
    /* m dict */
    off += snprintf((char*)buf+off, sizeof(buf)-off, "1:md");
    off += snprintf((char*)buf+off, sizeof(buf)-off, "6:ut_pexi%de", EXT_PEX_ID);
    buf[off++] = 'e'; /* end m */
    off += snprintf((char*)buf+off, sizeof(buf)-off, "1:pi%ue", (unsigned)listen_port);
    off += snprintf((char*)buf+off, sizeof(buf)-off, "11:upload_onlyi0e");
    buf[off++] = 'e'; /* end outer dict */
    int r = send_msg(p, buf, (uint32_t)off);
    p->ext_handshake_sent = 1;
    return r;
}

/* --- parse extension handshake --- */
static void parse_ext_handshake(peer_t *p, const uint8_t *data, uint32_t len) {
    const char *d = (const char*)data;
    const char *end = d + len;
    if (d >= end || *d != 'd') return;

    be_node_t m_node;
    if (be_dict_get(d, end, "m", 1, &m_node) && m_node.type == BE_DICT) {
        be_node_t pex_id;
        if (be_dict_get(m_node.buf, m_node.buf + m_node.raw_len, "ut_pex", 6, &pex_id)
            && pex_id.type == BE_INT) {
            p->peer_ext_pex = (uint8_t)pex_id.ival;
        }
    }
}

/* --- parse ut_pex --- */
static void parse_pex(peer_t *p __attribute__((unused)), const uint8_t *data, uint32_t len,
                      void (*on_peers)(void *ud, const uint8_t *compact, uint32_t cnt),
                      void *ud) {
    const char *d = (const char*)data;
    const char *end = d + len;
    be_node_t added;
    if (be_dict_get(d, end, "added", 5, &added) && added.type == BE_STR) {
        uint32_t cnt = (uint32_t)(added.slen / 6);
        if (cnt > 0 && on_peers)
            on_peers(ud, (const uint8_t*)added.sval, cnt);
    }
}

/* --- send ut_pex --- */
int peer_send_pex(peer_t *p, const uint8_t *compact6, uint32_t cnt) {
    if (!p->peer_ext_pex || cnt == 0) return 1;
    /* Build extended msg: [MSG_EXTENDED][peer_ext_pex id][bencode] */
    uint8_t buf[512];
    size_t off = 0;
    buf[off++] = MSG_EXTENDED;
    buf[off++] = p->peer_ext_pex;
    /* d5:added<6*cnt>:...7:added.f<cnt>:...e */
    off += snprintf((char*)buf+off, sizeof(buf)-off, "d5:added%u:", cnt*6);
    if (off + cnt*6 + 64 > sizeof(buf)) return 0;
    memcpy(buf+off, compact6, cnt*6);
    off += cnt*6;
    /* added.f — flags bytes, all 0 */
    off += snprintf((char*)buf+off, sizeof(buf)-off, "7:added.f%u:", cnt);
    memset(buf+off, 0, cnt);
    off += cnt;
    buf[off++] = 'e';
    return send_msg(p, buf, (uint32_t)off);
}

/* --- bitfield / interested --- */
int peer_send_bitfield(peer_t *p, const uint8_t *bf, uint32_t bf_bytes) {
    uint8_t *buf = (uint8_t*)malloc(1 + bf_bytes);
    if (!buf) return 0;
    buf[0] = MSG_BITFIELD;
    memcpy(buf+1, bf, bf_bytes);
    int r = send_msg(p, buf, 1 + bf_bytes);
    free(buf);
    return r;
}

int peer_send_interested(peer_t *p) {
    p->am_interested = 1;
    return send_byte_msg(p, MSG_INTERESTED);
}

/* --- request block --- */
int peer_request_block(peer_t *p, uint32_t piece, uint32_t offset, uint32_t len) {
    if (p->pipeline_len >= MAX_PIPELINE) return 0;
    uint8_t buf[13];
    buf[0] = MSG_REQUEST;
    buf[1] = (piece>>24)&0xFF; buf[2]=(piece>>16)&0xFF;
    buf[3] = (piece>> 8)&0xFF; buf[4]=(piece    )&0xFF;
    buf[5] = (offset>>24)&0xFF; buf[6]=(offset>>16)&0xFF;
    buf[7] = (offset>> 8)&0xFF; buf[8]=(offset    )&0xFF;
    buf[9] = (len>>24)&0xFF; buf[10]=(len>>16)&0xFF;
    buf[11]= (len>> 8)&0xFF; buf[12]=(len    )&0xFF;
    if (!send_msg(p, buf, 13)) return 0;
    block_req_t *req = &p->pipeline[p->pipeline_len++];
    req->index  = (int)piece;
    req->offset = (int)offset;
    req->length = (int)len;
    req->requested_ms = now_ms();
    return 1;
}

int peer_expire_requests(peer_t *p, uint64_t now, uint64_t timeout_ms,
                         void (*on_expired)(void*, const block_req_t*),
                         void *ud) {
    int kept = 0;
    int expired = 0;
    for (int i = 0; i < p->pipeline_len; i++) {
        block_req_t req = p->pipeline[i];
        if (req.requested_ms <= now &&
            now - req.requested_ms >= timeout_ms) {
            if (on_expired)
                on_expired(ud, &req);
            expired++;
            continue;
        }
        p->pipeline[kept++] = req;
    }
    p->pipeline_len = kept;
    return expired;
}

/* --- recv/dispatch --- */
static int process_handshake(peer_t *p, const peer_ctx_t *ctx) {
    if (p->rbuf_len - p->rbuf_head < BT_HANDSHAKE_LEN) return 0; /* wait */
    uint8_t *hs = p->rbuf + p->rbuf_head;
    if (hs[0] != 19 || memcmp(hs+1, "BitTorrent protocol", 19) != 0) {
        log_msg("[peer] bad handshake\n");
        return -1;
    }
    if (memcmp(hs+28, ctx->info_hash, 20) != 0) {
        log_msg("[peer] info_hash mismatch\n");
        return -1;
    }
    /* Check extension bit (byte 25 of reserved = hs[25], bit 4) */
    p->supports_ext = (hs[25] & 0x10) != 0;
    /* Consume handshake */
    p->rbuf_head += BT_HANDSHAKE_LEN;
    p->state = PS_ACTIVE;
    p->last_recv_ms = now_ms();
    log_msg("[peer] handshake ok ext=%d\n", p->supports_ext);
    return 1; /* keep processing */
}

/* Returns the removed request's requested_ms, or 0 if it was not found. */
static uint64_t remove_pipeline(peer_t *p, int piece, int offset) {
    for (int i = 0; i < p->pipeline_len; i++) {
        if (p->pipeline[i].index == piece && p->pipeline[i].offset == offset) {
            uint64_t requested_ms = p->pipeline[i].requested_ms;
            memmove(&p->pipeline[i], &p->pipeline[i+1],
                    (p->pipeline_len - i - 1) * sizeof(block_req_t));
            p->pipeline_len--;
            return requested_ms;
        }
    }
    return 0;
}

int peer_cancel_block(peer_t *p, uint32_t piece, uint32_t offset,
                      uint32_t len) {
    int found = 0;
    for (int i = 0; i < p->pipeline_len; ++i) {
        if (p->pipeline[i].index == (int)piece &&
            p->pipeline[i].offset == (int)offset) {
            found = 1;
            break;
        }
    }
    if (!found)
        return 0;

    uint8_t buf[13];
    buf[0] = MSG_CANCEL;
    buf[1] = (piece>>24)&0xFF; buf[2]=(piece>>16)&0xFF;
    buf[3] = (piece>> 8)&0xFF; buf[4]=(piece    )&0xFF;
    buf[5] = (offset>>24)&0xFF; buf[6]=(offset>>16)&0xFF;
    buf[7] = (offset>> 8)&0xFF; buf[8]=(offset    )&0xFF;
    buf[9] = (len>>24)&0xFF; buf[10]=(len>>16)&0xFF;
    buf[11]= (len>> 8)&0xFF; buf[12]=(len    )&0xFF;
    if (!send_msg(p, buf, sizeof(buf)))
        return 0;
    remove_pipeline(p, (int)piece, (int)offset);
    return 1;
}

static int process_message(peer_t *p, const peer_ctx_t *ctx,
                           void (*on_block)(void*, uint32_t, uint32_t, const uint8_t*, uint32_t),
                           void (*on_have)(void*, uint32_t),
                           void (*on_peers)(void*, const uint8_t*, uint32_t),
                           void *ud) {
    uint32_t avail = p->rbuf_len - p->rbuf_head;
    if (avail < 4) return 0;
    const uint8_t *base = p->rbuf + p->rbuf_head;
    uint32_t msg_len = ((uint32_t)base[0]<<24)|((uint32_t)base[1]<<16)|
                       ((uint32_t)base[2]<<8 )| (uint32_t)base[3];
    if (msg_len > 1<<18) { /* 256KB max message */
        log_msg("[peer] oversized message %u\n", msg_len);
        return -1;
    }
    if (avail < 4 + msg_len) return 0; /* wait */

    const uint8_t *msg = base + 4;
    if (msg_len == 0) {
        /* keepalive */
        p->rbuf_head += 4;
        return 1;
    }
    uint8_t id = msg[0];
    const uint8_t *payload = msg + 1;
    uint32_t plen = msg_len - 1;

    switch (id) {
    case MSG_CHOKE:
        p->am_choked = 1;
        break;
    case MSG_UNCHOKE:
        if (p->am_choked)
            log_msg("[peer] unchoked\n");
        p->am_choked = 0;
        break;
    case MSG_INTERESTED:
        p->peer_interested = 1;
        break;
    case MSG_NOT_INTEREST:
        p->peer_interested = 0;
        break;
    case MSG_HAVE:
        if (plen >= 4) {
            uint32_t idx = ((uint32_t)payload[0]<<24)|((uint32_t)payload[1]<<16)|
                           ((uint32_t)payload[2]<<8 )| (uint32_t)payload[3];
            if (idx < ctx->num_pieces && idx/8 < p->bf_bytes)
                bf_set(p->bitfield, idx);
            if (on_have) on_have(ud, idx);
        }
        break;
    case MSG_BITFIELD:
        /* BEP3: bitfield may only be sent right after handshake. We already
           sent ours (+ interested) in peer_recv on handshake completion. Just
           record what pieces the peer has. */
        if (plen <= p->bf_bytes) {
            memcpy(p->bitfield, payload, plen);
        }
        break;
    case MSG_PIECE:
        if (plen >= 8) {
            uint32_t idx = ((uint32_t)payload[0]<<24)|((uint32_t)payload[1]<<16)|
                           ((uint32_t)payload[2]<<8 )| (uint32_t)payload[3];
            uint32_t off = ((uint32_t)payload[4]<<24)|((uint32_t)payload[5]<<16)|
                           ((uint32_t)payload[6]<<8 )| (uint32_t)payload[7];
            uint32_t blen = plen - 8;
            uint64_t requested_ms = remove_pipeline(p, (int)idx, (int)off);
            p->downloaded += blen;
            p->telemetry_piece_bytes += blen;
            p->last_piece_ms = now_ms();
            /* Latency sample only for blocks we actually asked this peer for
               (expired/cancelled requests return 0). Clamped to >= 1 ms so a
               populated EMA is distinguishable from "no sample yet". */
            if (requested_ms && requested_ms <= p->last_piece_ms) {
                uint32_t latency = (uint32_t)(p->last_piece_ms - requested_ms);
                if (latency == 0)
                    latency = 1;
                p->block_lat_ema_ms = p->block_lat_ema_ms
                    ? (uint32_t)((int32_t)p->block_lat_ema_ms +
                                 ((int32_t)latency -
                                  (int32_t)p->block_lat_ema_ms) * 3 / 10)
                    : latency;
            }
            if (p->timeout_strikes > 0)
                p->timeout_strikes--;
            if (on_block) on_block(ud, idx, off, payload+8, blen);
        }
        break;
    case MSG_EXTENDED:
        if (plen >= 1) {
            uint8_t ext_id = payload[0];
            if (ext_id == EXT_HANDSHAKE_ID) {
                parse_ext_handshake(p, payload+1, plen-1);
            } else if (p->peer_ext_pex && ext_id == p->peer_ext_pex) {
                parse_pex(p, payload+1, plen-1, on_peers, ud);
            }
        }
        break;
    case MSG_PORT:
        /* DHT port announcement — could seed DHT, we ignore for now */
        break;
    default:
        break;
    }

    p->last_recv_ms = now_ms();
    p->rbuf_head += 4 + msg_len;
    return 1;
}

int peer_recv(peer_t *p, const peer_ctx_t *ctx,
              void (*on_block)(void*, uint32_t, uint32_t, const uint8_t*, uint32_t),
              void (*on_have)(void*, uint32_t),
              void (*on_peers)(void*, const uint8_t*, uint32_t),
              void *ud) {
    /* Check connect completion */
    if (p->state == PS_CONNECTING) {
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) {
            log_msg("[peer] connect failed: %s\n", strerror(err));
            return -1;
        }
        p->state = PS_HANDSHAKE;
        if (!peer_send_handshake(p, ctx)) return -1;
        /* Don't try to read yet */
        return 0;
    }

    for (;;) {
        if (p->state == PS_HANDSHAKE) {
            int r = process_handshake(p, ctx);
            if (r < 0) return -1;
            if (r > 0) {
                net_set_tcp_receive_buffer(p->fd);
                if (p->supports_ext)
                    peer_send_ext_handshake(p, ctx->listen_port);
                peer_send_bitfield(p, ctx->our_bf, ctx->bf_bytes);
                peer_send_interested(p);
            }
        }

        while (p->state == PS_ACTIVE) {
            int r = process_message(p, ctx, on_block, on_have, on_peers, ud);
            if (r < 0) return -1;
            if (r == 0) break;
        }

        /* Fully drained: reset the cursor to the front for free (no copy). */
        if (p->rbuf_head == p->rbuf_len) {
            p->rbuf_head = 0;
            p->rbuf_len  = 0;
        }
        size_t space = sizeof(p->rbuf) - p->rbuf_len;
        if (space == 0) {
            /* Tail is full but a partial message sits past rbuf_head; slide it
               to the front to reclaim the consumed prefix, then retry. */
            if (p->rbuf_head > 0) {
                memmove(p->rbuf, p->rbuf + p->rbuf_head,
                        p->rbuf_len - p->rbuf_head);
                p->rbuf_len -= p->rbuf_head;
                p->rbuf_head = 0;
                space = sizeof(p->rbuf) - p->rbuf_len;
            }
            if (space == 0) {
                log_msg("[peer] recv buffer full\n");
                return -1;
            }
        }
        int n = net_recv(p->fd, p->rbuf + p->rbuf_len, space);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1; /* EOF */
        p->rbuf_len += (uint32_t)n;
    }
}
